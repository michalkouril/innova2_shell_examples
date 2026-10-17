// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// innova2_hostmem (tier-1 host memory access experiment, 2026-10-02): allocates NBUF DMA-coherent 1 MB host buffers
// against the FPGA user PF so an RP kernel can reach them through a peer_window window (the XDMA AXI slave bridge
// turns its AXI reads/writes into PCIe memory requests addressed to host RAM).
//   insmod innova2_hostmem.ko bdf=0000:04:00.1
//   /dev/innova2_hostmem: ioctl INNOVA2_HOSTMEM_INFO -> {nbuf, size, bus[NBUF]}; mmap offset k*1MB = buffer k.
// Without an IOMMU the bus address is the physical address; with one, it is the IOVA mapped for this device (the
// bridge's requester must then be the same function -- not verified, the test host runs without an IOMMU).
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#define NBUF 3
#define BUFSZ (1u << 20)
struct hostmem_info { __u32 nbuf, size; __u64 bus[NBUF]; };
#define INNOVA2_HOSTMEM_INFO _IOR('h', 1, struct hostmem_info)

static char *bdf = "0000:04:00.1";
module_param(bdf, charp, 0444);
MODULE_PARM_DESC(bdf, "PCI function the buffers are mapped for (FPGA user PF)");

static struct pci_dev *pdev;
static void *cpu[NBUF];
static dma_addr_t bus[NBUF];

static long hm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct hostmem_info in = { .nbuf = NBUF, .size = BUFSZ };
	int i;
	if (cmd != INNOVA2_HOSTMEM_INFO) return -ENOTTY;
	for (i = 0; i < NBUF; i++) in.bus[i] = bus[i];
	return copy_to_user((void __user *)arg, &in, sizeof(in)) ? -EFAULT : 0;
}

static int hm_mmap(struct file *f, struct vm_area_struct *vma)
{
	const unsigned long k = vma->vm_pgoff >> (20 - PAGE_SHIFT), len = vma->vm_end - vma->vm_start;
	if (vma->vm_pgoff & ((BUFSZ >> PAGE_SHIFT) - 1) || k >= NBUF || len > BUFSZ) return -EINVAL;
	vma->vm_pgoff = 0;
	return dma_mmap_coherent(&pdev->dev, vma, cpu[k], bus[k], len);
}

static const struct file_operations hm_fops = { .owner = THIS_MODULE, .unlocked_ioctl = hm_ioctl, .mmap = hm_mmap };
static struct miscdevice hm_dev = { .minor = MISC_DYNAMIC_MINOR, .name = "innova2_hostmem", .fops = &hm_fops, .mode = 0600 };

static int __init hm_init(void)
{
	unsigned int dom, b, d, fn;
	int i, rc;
	if (sscanf(bdf, "%x:%x:%x.%x", &dom, &b, &d, &fn) != 4) return -EINVAL;
	pdev = pci_get_domain_bus_and_slot(dom, b, PCI_DEVFN(d, fn));
	if (!pdev) { pr_err("innova2_hostmem: %s not found\n", bdf); return -ENODEV; }
	for (i = 0; i < NBUF; i++) {
		cpu[i] = dma_alloc_coherent(&pdev->dev, BUFSZ, &bus[i], GFP_KERNEL);
		if (!cpu[i]) { rc = -ENOMEM; goto fail; }
		memset(cpu[i], 0, BUFSZ);
		pr_info("innova2_hostmem: buffer %d bus 0x%llx (1 MB, %s)\n", i, (unsigned long long)bus[i], bdf);
	}
	rc = misc_register(&hm_dev);
	if (!rc) return 0;
fail:
	while (--i >= 0) dma_free_coherent(&pdev->dev, BUFSZ, cpu[i], bus[i]);
	pci_dev_put(pdev);
	return rc;
}

static void __exit hm_exit(void)
{
	int i;
	misc_deregister(&hm_dev);
	for (i = 0; i < NBUF; i++) if (cpu[i]) dma_free_coherent(&pdev->dev, BUFSZ, cpu[i], bus[i]);
	pci_dev_put(pdev);
}
module_init(hm_init);
module_exit(hm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("innova2: DMA-coherent host buffers for RP kernels through peer_window (experiment)");
