// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: GPL-2.0-only
/*
 * innova2_peermem -- RDMA peer-memory client for XRT P2P buffers on Innova-2 (and any xocl device).
 *
 * An XRT P2P buffer object lives in FPGA DDR and is reached through the user PF's BAR4. xocl maps it into user space
 * (drm_gem_mmap, pfns inserted on fault) but its pages are not a registered pci_p2pdma provider, so neither
 * get_user_pages-based ibv_reg_mr nor a dma-buf import can hand it to an RDMA device. This client lets MLNX/DOCA-OFED's
 * ib_core (ib_register_peer_memory_client, the GPUDirect hook) map such a range for the ConnectX:
 *
 *  acquire   the range must lie in ONE vma of an xocl file, and every page must resolve to a pfn inside BAR4 of a
 *            10ee:6a90 (Innova-2 user PF) device -- anything else is "not ours" (return 0), so ordinary memory is never
 *            touched. Non-present pages are faulted in first.
 *  get_pages takes a reference on the GEM object behind the vma, so the BO cannot be freed while the RDMA device holds
 *            a registration (a freed BO would otherwise leave the NIC writing into reallocated FPGA memory).
 *  dma_map   bus address = pci_bus_address(BAR4) + offset, merged into contiguous segments. Refused if the importing
 *            device sits behind a translating IOMMU (the bus address would be wrong), which is not the case on the
 *            test hosts (no IOMMU, ACS off inside the card's switch).
 *
 * Proof of concept: no invalidation support (registered with a NULL invalidate callback), no BAR-resize handling --
 * do not resize BAR4 (warm reboot) while registrations exist; they do not survive a reboot anyway.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/iommu.h>
#include <drm/drm_gem.h>
#include <rdma/peer_mem.h>
#include <linux/version.h>
#include <linux/sched/mm.h>

/* Older kernels (x440: 5.8 + MLNX_OFED 5.2). vma_lookup arrived in 5.14; fixup_user_fault lost its task argument in
 * 5.9; follow_pte(mm, addr, ptep, ptl) is exported from 5.12 -- before that use the exported follow_pfn(), which
 * covers exactly our case (xocl maps the P2P BAR with VM_PFNMAP). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
static inline struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *v = find_vma(mm, addr);

	return (v && addr >= v->vm_start) ? v : NULL;
}
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
#define ipm_fixup_user_fault(mm, va, flags) fixup_user_fault(current, (mm), (va), (flags), NULL)
#else
#define ipm_fixup_user_fault(mm, va, flags) fixup_user_fault((mm), (va), (flags), NULL)
#endif

#define DRV_NAME "innova2_peermem"
#define XLNX_VENDOR 0x10ee
#define XLNX_USER_PF 0x6a90
#define P2P_BAR 4

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "log every acquire/map (1)");

struct ipm_ctx {
	unsigned long start;            /* page-aligned user VA */
	unsigned long npages;
	unsigned long *pfns;
	struct pci_dev *pdev;           /* owner of BAR4, referenced */
	struct drm_gem_object *obj;     /* referenced between get_pages and put_pages */
	struct mm_struct *mm;
};

static void *reg_handle;

static struct pci_dev *bar4_owner(phys_addr_t phys)
{
	struct pci_dev *p = NULL;

	while ((p = pci_get_device(XLNX_VENDOR, XLNX_USER_PF, p))) {
		resource_size_t s = pci_resource_start(p, P2P_BAR), l = pci_resource_len(p, P2P_BAR);

		if (l && phys >= s && phys < s + l)
			return p;       /* reference held by pci_get_device */
	}
	return NULL;
}

static bool is_xocl_vma(struct vm_area_struct *vma)
{
	struct file *f = vma->vm_file;

	return f && f->f_op && f->f_op->owner && !strcmp(module_name(f->f_op->owner), "xocl");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static int pte_pfn_cb(pte_t *ptep, unsigned long addr, void *data)
{
	pte_t pte = ptep_get(ptep);

	if (!pte_present(pte))
		return -EFAULT;
	*(unsigned long *)data = pte_pfn(pte);
	return 0;
}
#endif

/* the pfn behind one user page; faults it in if needed. mmap lock held (read). */
static int va_to_pfn(struct mm_struct *mm, unsigned long va, unsigned long *pfn)
{
	int tries;

	for (tries = 0; tries < 2; tries++) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
		struct vm_area_struct *vma = vma_lookup(mm, va);

		if (vma && !follow_pfn(vma, va, pfn))
			return 0;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
		/* follow_pte() was removed in 6.12 (Ubuntu 26.04 runs 7.0): follow_pfnmap_start/end replace it */
		struct follow_pfnmap_args args = { .vma = vma_lookup(mm, va), .address = va };

		if (args.vma && !follow_pfnmap_start(&args)) {
			*pfn = args.pfn;
			follow_pfnmap_end(&args);
			return 0;
		}
		/* follow_pfnmap_start() takes only VM_IO/VM_PFNMAP vmas; XRT maps P2P BOs VM_MIXEDMAP (vmf_insert_mixed ->
		 * special ptes, which GUP will not follow either). Read the pte the way follow_pte() did: apply_to_page_range
		 * (exported; apply_to_existing_page_range is not) runs the callback under the pte lock */
		if (args.vma && (args.vma->vm_flags & VM_MIXEDMAP) && !apply_to_page_range(mm, va, PAGE_SIZE, pte_pfn_cb, pfn))
			return 0;
#else
		pte_t *ptep;
		spinlock_t *ptl;

		if (!follow_pte(mm, va, &ptep, &ptl)) {
			pte_t pte = ptep_get(ptep);
			bool ok = pte_present(pte);

			if (ok)
				*pfn = pte_pfn(pte);
			pte_unmap_unlock(ptep, ptl);
			if (ok)
				return 0;
		}
#endif
		if (ipm_fixup_user_fault(mm, va, FAULT_FLAG_WRITE))
			return -EFAULT;
	}
	return -EFAULT;
}

static int ipm_acquire(unsigned long addr, size_t size, void *priv, char *name, void **client_context)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct ipm_ctx *c;
	unsigned long start = addr & PAGE_MASK, end = PAGE_ALIGN(addr + size), i;
	struct pci_dev *pdev = NULL;
	int ret = 0;

	if (!mm || !size)
		return 0;
	mmap_read_lock(mm);
	vma = vma_lookup(mm, start);
	if (!vma || end > vma->vm_end || !is_xocl_vma(vma)) {
		if (debug)
			pr_info(DRV_NAME ": skip va 0x%lx +0x%zx: %s\n", addr, size, !vma ? "no vma" :
				end > vma->vm_end ? "spans vmas" : "not an xocl mapping");
		goto out_unlock;                        /* not ours */
	}
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		goto out_unlock;
	c->start = start;
	c->npages = (end - start) >> PAGE_SHIFT;
	c->pfns = kvmalloc_array(c->npages, sizeof(*c->pfns), GFP_KERNEL);
	if (!c->pfns)
		goto out_free;
	for (i = 0; i < c->npages; i++) {
		struct pci_dev *o;

		if (va_to_pfn(mm, start + (i << PAGE_SHIFT), &c->pfns[i])) {
			if (debug)
				pr_info(DRV_NAME ": skip va 0x%lx: no pfn for page %lu (vm_flags 0x%lx)\n", addr, i,
					(unsigned long)vma->vm_flags);
			goto out_free;
		}
		o = bar4_owner(PFN_PHYS(c->pfns[i]));
		if (!o) {
			if (debug)
				pr_info(DRV_NAME ": skip va 0x%lx: page %lu phys 0x%llx is not in a BAR4\n", addr, i,
					(unsigned long long)PFN_PHYS(c->pfns[i]));
			goto out_free;                  /* an xocl mapping, but not P2P BAR memory */
		}
		if (!pdev) {
			pdev = o;
		} else {
			pci_dev_put(o);
			if (o != pdev)
				goto out_free;          /* range spans two cards */
		}
	}
	c->pdev = pdev;
	c->mm = mm;
	mmgrab(mm);
	*client_context = c;
	ret = 1;
	if (debug)
		pr_info(DRV_NAME ": acquire va 0x%lx +0x%zx -> %s BAR4 phys 0x%llx (%lu pages)\n", addr, size,
			pci_name(pdev), (unsigned long long)PFN_PHYS(c->pfns[0]), c->npages);
	mmap_read_unlock(mm);
	return ret;

out_free:
	if (pdev)
		pci_dev_put(pdev);
	kvfree(c->pfns);
	kfree(c);
out_unlock:
	mmap_read_unlock(mm);
	return 0;
}

static int ipm_get_pages(unsigned long addr, size_t size, int write, int force, struct sg_table *sg_head,
			 void *client_context, u64 core_context)
{
	struct ipm_ctx *c = client_context;
	struct vm_area_struct *vma;
	int ret = -EFAULT;

	mmap_read_lock(c->mm);
	vma = vma_lookup(c->mm, c->start);
	if (vma && is_xocl_vma(vma) && vma->vm_private_data) {
		c->obj = vma->vm_private_data;  /* drm_gem_mmap() stores the GEM object here */
		drm_gem_object_get(c->obj);
		ret = 0;
	}
	mmap_read_unlock(c->mm);
	return ret;
}

static int ipm_dma_map(struct sg_table *sg_head, void *client_context, struct device *dma_device, int dmasync,
		       int *nmap)
{
	struct ipm_ctx *c = client_context;
	resource_size_t cpu0 = pci_resource_start(c->pdev, P2P_BAR);
	pci_bus_addr_t bus0 = pci_bus_address(c->pdev, P2P_BAR);
	struct scatterlist *sg;
	unsigned long i, nseg = 1;
	int ret;

	if (device_iommu_mapped(dma_device) && iommu_get_domain_for_dev(dma_device) &&
	    iommu_get_domain_for_dev(dma_device)->type != IOMMU_DOMAIN_IDENTITY) {
		dev_warn(dma_device, DRV_NAME ": translating IOMMU in front of the importer -- refusing\n");
		return -EOPNOTSUPP;
	}
	for (i = 1; i < c->npages; i++)
		if (c->pfns[i] != c->pfns[i - 1] + 1)
			nseg++;
	ret = sg_alloc_table(sg_head, nseg, GFP_KERNEL);
	if (ret)
		return ret;
	sg = sg_head->sgl;
	for (i = 0; i < c->npages; ) {
		unsigned long j = i + 1;

		while (j < c->npages && c->pfns[j] == c->pfns[j - 1] + 1)
			j++;
		sg->length = (j - i) << PAGE_SHIFT;
		sg->offset = 0;
		sg_dma_address(sg) = bus0 + (PFN_PHYS(c->pfns[i]) - cpu0);
		sg_dma_len(sg) = sg->length;
		sg = sg_next(sg);
		i = j;
	}
	*nmap = nseg;
	if (debug)
		pr_info(DRV_NAME ": dma_map for %s: %lu segment(s), first bus 0x%llx\n", dev_name(dma_device), nseg,
			(unsigned long long)sg_dma_address(sg_head->sgl));
	return 0;
}

static int ipm_dma_unmap(struct sg_table *sg_head, void *client_context, struct device *dma_device)
{
	return 0;       /* nothing was mapped through the DMA API */
}

static void ipm_put_pages(struct sg_table *sg_head, void *client_context)
{
	struct ipm_ctx *c = client_context;

	sg_free_table(sg_head);
	if (c->obj) {
		drm_gem_object_put(c->obj);
		c->obj = NULL;
	}
}

static unsigned long ipm_get_page_size(void *client_context)
{
	return PAGE_SIZE;
}

static void ipm_release(void *client_context)
{
	struct ipm_ctx *c = client_context;

	pci_dev_put(c->pdev);
	mmdrop(c->mm);
	kvfree(c->pfns);
	kfree(c);
}

static const struct peer_memory_client ipm_client = {
	.name = DRV_NAME,
	.version = "0.1",
	.acquire = ipm_acquire,
	.get_pages = ipm_get_pages,
	.dma_map = ipm_dma_map,
	.dma_unmap = ipm_dma_unmap,
	.put_pages = ipm_put_pages,
	.get_page_size = ipm_get_page_size,
	.release = ipm_release,
};

static int __init ipm_init(void)
{
	reg_handle = ib_register_peer_memory_client(&ipm_client, NULL);
	if (!reg_handle)
		return -EINVAL;
	pr_info(DRV_NAME ": registered (XRT P2P BAR%d of %04x:%04x)\n", P2P_BAR, XLNX_VENDOR, XLNX_USER_PF);
	return 0;
}

static void __exit ipm_exit(void)
{
	ib_unregister_peer_memory_client(reg_handle);
}

module_init(ipm_init);
module_exit(ipm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RDMA peer-memory client for XRT P2P buffers (Innova-2)");
