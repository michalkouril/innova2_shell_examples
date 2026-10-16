// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E1 (2026-09-29): a raw-Ethernet send queue whose WQE ring, doorbell record AND packet data all live in FPGA DDR
// (one XRT P2P buffer object, reached through BAR4; registered via the innova2_peermem peer-memory client). The CPU
// writes WQEs into FPGA memory and rings the UAR doorbell; the ConnectX fetches the WQEs and the payload from the FPGA
// over PCIe peer-to-peer and transmits on port 0. Port 1 (cable loop) receives into HOST memory with a plain verbs raw
// QP, and the host compares byte for byte. The TX completion queue is a verbs CQ in host memory, polled directly.
//   --host   control: the same SQ/WQ/DBR/data layout in a host buffer instead of FPGA DDR.
//   usage (root): e1_sq <user BDF> <bank0 xclbin> <tx dev> <rx dev> [count<=64] [--host]
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "devx_sq.h"

static const size_t FRAME = 1024, SLOTS = 64, BUF = 4 << 20;
static const size_t DATA_OFF = 0, RING_OFF = 0x40000, RING_LEN = 0x20000, WQ_OFF = 0, DBR_OFF = 0x10000;
static const int LOG_WQ = 6;                        // 64 WQEBBs
static const uint16_t ETYPE = 0x88B5;

static ibv_context *open_dev(const char *name, bool devx)
{
  int n = 0; ibv_device **l = ibv_get_device_list(&n); ibv_context *c = nullptr;
  for (int i = 0; i < n; i++)
    if (!strcmp(ibv_get_device_name(l[i]), name)) {
      if (devx) { mlx5dv_context_attr a{}; a.flags = MLX5DV_CONTEXT_FLAGS_DEVX; c = mlx5dv_open_device(l[i], &a); }
      else c = ibv_open_device(l[i]);
    }
  ibv_free_device_list(l); return c;
}

static ibv_qp *raw_rx_qp(ibv_pd *pd, ibv_cq *cq)
{
  ibv_qp_init_attr ia{}; ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = 1; ia.cap.max_recv_wr = SLOTS; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia); if (!qp) { perror("rx create_qp"); exit(1); }
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1;
  if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT)) { perror("INIT"); exit(1); }
  a = {}; a.qp_state = IBV_QPS_RTR; if (ibv_modify_qp(qp, &a, IBV_QP_STATE)) { perror("RTR"); exit(1); }
  return qp;
}

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <tx dev> <rx dev> [count] [--host]\n", argv[0]); return 2; }
  bool host = !strcmp(argv[argc - 1], "--host");
  int count = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 16; if (count > (int)SLOTS) count = SLOTS;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  dev.load_xclbin(std::string(argv[2]));
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, 0);
  uint8_t *base = host ? (uint8_t *)aligned_alloc(4096, BUF) : bo.map<uint8_t *>();
  memset(base + RING_OFF, 0, RING_LEN);
  uint8_t *data = base + DATA_OFF, *wq = base + RING_OFF + WQ_OFF;
  volatile uint32_t *dbr = (volatile uint32_t *)(base + RING_OFF + DBR_OFF);
  printf("SQ ring, doorbell record and packet data in %s\n", host ? "HOST memory (control)" : "FPGA DDR (XRT P2P BO via BAR4)");

  ibv_context *ct = open_dev(argv[3], true), *cr = open_dev(argv[4], false);
  if (!ct || !cr) { fprintf(stderr, "cannot open %s (DevX) / %s\n", argv[3], argv[4]); return 1; }
  ibv_pd *pd = ibv_alloc_pd(ct);
  ibv_mr *dmr = ibv_reg_mr(pd, data, SLOTS * FRAME, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *ring = mlx5dv_devx_umem_reg(ct, base + RING_OFF, RING_LEN, IBV_ACCESS_LOCAL_WRITE);
  printf("data MR: %s   ring umem: %s\n", dmr ? "ok" : strerror(errno), ring ? "ok" : strerror(errno));
  if (!dmr || !ring) return 1;
  ibv_cq *cq = ibv_create_cq(ct, 2 * SLOTS, nullptr, nullptr, 0);
  mlx5dv_pd dpd{}; mlx5dv_cq dcq{}; mlx5dv_obj o{};
  o.pd.in = pd; o.pd.out = &dpd; o.cq.in = cq; o.cq.out = &dcq;
  if (mlx5dv_init_obj(&o, MLX5DV_OBJ_PD | MLX5DV_OBJ_CQ)) { perror("init_obj"); return 1; }
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(ct, MLX5DV_UAR_ALLOC_TYPE_NC);
  if (!uar) { perror("alloc_uar"); return 1; }
  int mi = e1_query_min_inline(ct, err, sizeof(err));
  printf("pdn 0x%x  cqn 0x%x (%u CQEs of %u B)  uar page 0x%x  min_wqe_inline_mode %d%s%s\n", dpd.pdn, dcq.cqn,
         dcq.cqe_cnt, dcq.cqe_size, uar->page_id, mi, mi < 0 ? "  " : "", mi < 0 ? err : "");
  if (mi < 0) mi = 1;
  e1_sq sq;
  if (e1_create_sq(ct, dpd.pdn, dcq.cqn, uar->page_id, ring->umem_id, WQ_OFF, ring->umem_id, DBR_OFF, LOG_WQ, mi, &sq,
                   err, sizeof(err))) { printf("*** %s\n", err); return 1; }
  printf("DevX: tdn 0x%x  tisn 0x%x  sqn 0x%x  (RDY)\n", sq.tdn, sq.tisn, sq.sqn);

  // receiver on the other port: host memory, steered by EtherType
  ibv_pd *pr = ibv_alloc_pd(cr);
  uint8_t *rx = (uint8_t *)aligned_alloc(4096, SLOTS * FRAME); memset(rx, 0, SLOTS * FRAME);
  ibv_mr *rmr = ibv_reg_mr(pr, rx, SLOTS * FRAME, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq *rcq = ibv_create_cq(cr, 2 * SLOTS, nullptr, nullptr, 0);
  ibv_qp *rq = raw_rx_qp(pr, rcq);
  struct { ibv_flow_attr a; ibv_flow_spec_eth e; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 1; fl.a.port = 1;
  fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(ETYPE); fl.e.mask.ether_type = 0xFFFF;
  ibv_flow *flow = ibv_create_flow(rq, &fl.a);
  if (!flow) { perror("create_flow"); return 1; }
  for (int i = 0; i < count; i++) {
    ibv_sge s{(uint64_t)(rx + i * FRAME), (uint32_t)FRAME, rmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = i; w.sg_list = &s; w.num_sge = 1;
    if (ibv_post_recv(rq, &w, &bad)) { perror("post_recv"); return 1; }
  }

  // frames and WQEs are written by the host into (FPGA) memory; only the doorbell is an MMIO write to the ConnectX
  for (int i = 0; i < count; i++) {
    uint8_t *f = data + i * FRAME, tmp[FRAME];
    memset(tmp, 0xFF, 6); memcpy(tmp + 6, "\x02\x49\x4e\x4e\x32\x01", 6); tmp[12] = ETYPE >> 8; tmp[13] = ETYPE & 0xFF;
    for (size_t k = 14; k < FRAME; k++) tmp[k] = (uint8_t)(k * 5 + i * 29);
    tmp[14] = 0xE1; tmp[15] = i;
    memcpy(f, tmp, FRAME);
  }
  _mm_sfence();
  uint32_t pi = 0;
  for (int i = 0; i < count; i++, pi++) {
    uint8_t w[64] = {0}, *f = data + i * FRAME;
    auto *ctrl = (mlx5_wqe_ctrl_seg *)w; auto *eth = (mlx5_wqe_eth_seg *)(w + 16); auto *d = (mlx5_wqe_data_seg *)(w + 48);
    eth->inline_hdr_sz = htobe16(18);
    memcpy(eth->inline_hdr_start, f, 18);                    // L2 header inlined (min_wqe_inline_mode <= L2)
    mlx5dv_set_data_seg(d, FRAME - 18, dmr->lkey, (uintptr_t)(f + 18));
    mlx5dv_set_ctrl_seg(ctrl, pi & 0xffff, MLX5_OPCODE_SEND, 0, sq.sqn, MLX5_WQE_CTRL_CQ_UPDATE, 4, 0, 0);
    memcpy(wq + (pi & ((1u << LOG_WQ) - 1)) * 64, w, 64);   // WQE -> FPGA DDR
    _mm_sfence();
    dbr[MLX5_SND_DBR] = htobe32((pi + 1) & 0xffff);         // doorbell record -> FPGA DDR
    _mm_sfence();
    *(volatile uint64_t *)uar->reg_addr = *(uint64_t *)w;   // ring: 8 bytes of the ctrl segment
    _mm_sfence();
  }

  int sent = 0, got = 0, errs = 0; uint32_t ci = 0;
  for (int t = 0; t < 3000 && (sent < count || got < count); t++) {
    for (;;) {
      uint8_t *p = (uint8_t *)dcq.buf + (ci & (dcq.cqe_cnt - 1)) * dcq.cqe_size + (dcq.cqe_size == 128 ? 64 : 0);
      auto *cqe = (mlx5_cqe64 *)p; uint8_t op = cqe->op_own >> 4;
      if (op == MLX5_CQE_INVALID || (cqe->op_own & 1) != !!(ci & dcq.cqe_cnt)) break;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (op == MLX5_CQE_REQ_ERR) {
        auto *e = (mlx5_err_cqe *)p; errs++;
        printf("TX error CQE: syndrome 0x%x vendor 0x%x wqe_counter %u\n", e->syndrome, e->vendor_err_synd, be16toh(e->wqe_counter));
      }
      sent++; ci++; dcq.dbrec[0] = htobe32(ci & 0xffffff);
    }
    ibv_wc wc[16]; int n;
    while ((n = ibv_poll_cq(rcq, 16, wc)) > 0) for (int k = 0; k < n; k++) { got++; if (wc[k].status) { errs++; printf("RX wc %s\n", ibv_wc_status_str(wc[k].status)); } }
    usleep(1000);
  }
  int good = 0;
  for (int i = 0; i < count; i++) good += !memcmp(rx + i * FRAME, data + i * FRAME, FRAME);
  printf("WQE 0 as stored in %s: ", host ? "host" : "FPGA DDR"); for (int k = 0; k < 16; k++) printf("%02x", wq[k]); printf("\n");
  printf("TX CQEs %d/%d, RX %d/%d, errors %d, frames byte-exact %d/%d\n", sent, count, got, count, errs, good, count);
  bool pass = sent == count && got == count && !errs && good == count;
  printf("RESULT %s (%s)\n", pass ? "PASS" : "FAIL", host ? "host-memory control" : "E1: SQ ring + DBR + data in FPGA DDR");

  ibv_destroy_flow(flow); ibv_destroy_qp(rq); ibv_destroy_cq(rcq); ibv_dereg_mr(rmr); ibv_dealloc_pd(pr);
  e1_destroy_sq(&sq); mlx5dv_devx_free_uar(uar); ibv_destroy_cq(cq); mlx5dv_devx_umem_dereg(ring); ibv_dereg_mr(dmr);
  ibv_dealloc_pd(pd); ibv_close_device(ct); ibv_close_device(cr);
  return pass ? 0 : 1;
}
