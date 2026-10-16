// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E2 (2026-09-29): ConnectX-5 port 0 receives Ethernet frames into FPGA DDR -- RQ ring + doorbell record, CQ ring +
// doorbell record and the frame buffers all in one XRT P2P BO (bank0, BAR4; registered through innova2_peermem) --
// and the consumer is the e2_rx RP kernel: it polls CQEs, checksums frames and re-posts RQ slots by writing the
// doorbell records in DDR. The CPU only sets the queues up and generates traffic on port 1 (cable loop).
//   default      kernel is the consumer (needs e2_rx.xclbin; --v3: the dataflow kernel e2_rx3)
//   --hostpoll   the CPU does the kernel's job through the BAR4 mapping (validates the DevX objects; any bank0 xclbin)
//   --host       with --hostpoll: everything in host memory instead (control)
//   usage (root): e2_host <user BDF> <xclbin> <rx dev> <tx dev> [frames] [gap_us] [--hostpoll] [--host]
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "devx_rx.h"

// v3 layout (also used by --hostpoll and the v2 kernel): 1024-entry RQ, 2048-entry CQ, 2 MB of frame buffers
static const size_t BUF = 8 << 20, FRAME = 1024, STRIDE = 2048;
static const int LOG_RQ = 10, LOG_CQ = 11;
static const size_t DATA_OFF = 0, UMEM_OFF = 0x200000, UMEM_LEN = 0x30000;             // umem: [2 MB, 2 MB + 192 KB)
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, CQ_OFF = 0x8000, CQ_DBR = 0x28000;  // inside the umem
static const size_t STOP_OFF = 0x240000;                                                  // host->kernel 'done sending' flag
static const uint16_t ETYPE = 0x88B6;

static bool has(int argc, char **argv, const char *f) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], f)) return true; return false; }

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

static void make_frame(uint8_t *f, unsigned i)
{
  memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x02", 6); f[12] = ETYPE >> 8; f[13] = ETYPE & 0xFF;
  for (size_t k = 14; k < FRAME; k++) f[k] = (uint8_t)(k * 3 + i * 17);
  f[14] = 0xE2; f[15] = i & 0xFF; f[16] = (i >> 8) & 0xFF;
}
static uint32_t word_sum(const uint8_t *f) { uint32_t s = 0, w; for (size_t k = 0; k < FRAME; k += 4) { memcpy(&w, f + k, 4); s += w; } return s; }

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <rx dev> <tx dev> [frames] [gap_us] [--hostpoll] [--host]\n", argv[0]); return 2; }
  unsigned n = argc > 5 && argv[5][0] != '-' ? atoi(argv[5]) : 1000, gap = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 50;
  if (n > 65535) n = 65535;
  bool v3 = has(argc, argv, "--v3");
  bool hostpoll = has(argc, argv, "--hostpoll"), host = has(argc, argv, "--host") && hostpoll;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krnl;
  int grp = 0;
  if (!hostpoll) { krnl = xrt::kernel(dev, uuid, v3 ? "e2_rx3" : "e2_rx"); grp = krnl.group_id(0); }
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, grp);
  uint8_t *base = host ? (uint8_t *)aligned_alloc(4096, BUF) : bo.map<uint8_t *>();
  uint8_t *um = base + UMEM_OFF;
  printf("E2: RQ/CQ rings, doorbell records and frame buffers in %s; consumer = %s\n",
         host ? "HOST memory (control)" : "FPGA DDR (XRT P2P BO, bank0 via BAR4)", hostpoll ? "the CPU (--hostpoll)" : v3 ? "the e2_rx3 RP kernel (dataflow)" : "the e2_rx RP kernel");

  ibv_context *cr = open_dev(argv[3], true), *ct = open_dev(argv[4], false);
  if (!cr || !ct) { fprintf(stderr, "cannot open %s (DevX) / %s\n", argv[3], argv[4]); return 1; }
  ibv_pd *pd = ibv_alloc_pd(cr);
  ibv_mr *dmr = ibv_reg_mr(pd, base + DATA_OFF, (1u << LOG_RQ) * STRIDE, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *umem = mlx5dv_devx_umem_reg(cr, um, UMEM_LEN, IBV_ACCESS_LOCAL_WRITE);
  printf("frame MR: %s   ring umem: %s\n", dmr ? "ok" : strerror(errno), umem ? "ok" : strerror(errno));
  if (!dmr || !umem) return 1;
  mlx5dv_pd dpd{}; mlx5dv_obj o{}; o.pd.in = pd; o.pd.out = &dpd; mlx5dv_init_obj(&o, MLX5DV_OBJ_PD);
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(cr, MLX5DV_UAR_ALLOC_TYPE_NC);

  // initial ring state, written by the host once: CQEs invalid (opcode 0xF, owner 1), RQ fully posted
  memset(um, 0, UMEM_LEN);
  for (unsigned i = 0; i < (1u << LOG_CQ); i++) um[CQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_RQ); i++)
    mlx5dv_set_data_seg((mlx5_wqe_data_seg *)(um + RQ_OFF + i * 16), STRIDE, dmr->lkey, (uintptr_t)(base + DATA_OFF + i * STRIDE));
  _mm_sfence();
  e2_rx_cfg cfg{dpd.pdn, uar->page_id, umem->umem_id, CQ_OFF, CQ_DBR, RQ_OFF, RQ_DBR, LOG_CQ, LOG_RQ, ETYPE};
  e2_rx rx;
  if (e2_rx_create(cr, &cfg, &rx, err, sizeof(err))) { printf("*** %s\n", err); return 1; }
  *(volatile uint32_t *)(um + RQ_DBR) = htobe32(1u << LOG_RQ);   // all RQ slots posted
  _mm_sfence();
  printf("DevX: eqn %u  cqn 0x%x  rqn 0x%x  tirn 0x%x  steering EtherType 0x%04x -> TIR\n", rx.eqn, rx.cqn, rx.rqn, rx.tirn, ETYPE);

  // the consumer
  xrt::bo res; xrt::run run;
  const unsigned max_polls = 400000000u;
  if (!hostpoll) {
    res = xrt::bo(dev, (4 + 2 * n) * 4 + 4096, krnl.group_id(v3 ? 3 : 2));
    std::vector<uint32_t> z((4 + 2 * n) + 1024, 0); res.write(z.data()); res.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    *(volatile uint32_t *)(base + STOP_OFF) = 0; _mm_sfence();
    if (v3)
      run = krnl(bo, bo, bo, res, bo, (unsigned)(UMEM_OFF + CQ_OFF), (unsigned)LOG_CQ, (unsigned)(UMEM_OFF + CQ_DBR),
                 (unsigned)(UMEM_OFF + RQ_DBR), (unsigned)LOG_RQ, (unsigned)DATA_OFF, (unsigned)STRIDE, n, max_polls, (unsigned)STOP_OFF);
    else
      run = krnl(bo, bo, res, (unsigned)(UMEM_OFF + CQ_OFF), (unsigned)LOG_CQ, (unsigned)(UMEM_OFF + CQ_DBR),
                 (unsigned)(UMEM_OFF + RQ_DBR), (unsigned)LOG_RQ, (unsigned)DATA_OFF, (unsigned)STRIDE, n, max_polls, (unsigned)STOP_OFF);
    printf("e2_rx kernel started (BO device address 0x%llx)\n", (unsigned long long)bo.address());
  }

  // traffic generator on the other port: host memory, verbs raw QP
  ibv_pd *pt = ibv_alloc_pd(ct);
  uint8_t *tx = (uint8_t *)aligned_alloc(4096, 64 * FRAME);
  ibv_mr *tmr = ibv_reg_mr(pt, tx, 64 * FRAME, 0);
  ibv_cq *tcq = ibv_create_cq(ct, 128, nullptr, nullptr, 0);
  ibv_qp_init_attr ia{}; ia.send_cq = tcq; ia.recv_cq = tcq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = 64; ia.cap.max_recv_wr = 1; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *tq = ibv_create_qp(pt, &ia);
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(tq, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(tq, &a, IBV_QP_STATE);
  a = {}; a.qp_state = IBV_QPS_RTS; ibv_modify_qp(tq, &a, IBV_QP_STATE);

  std::vector<uint32_t> expect(n), seen; seen.reserve(n);
  unsigned sent = 0, inflight = 0, ci = 0, errs = 0, polls = 0;
  auto t0 = std::chrono::steady_clock::now();
  auto hostpoll_step = [&]() {                     // what the kernel does, done by the CPU through BAR4
    for (;;) {
      const uint8_t *cqe = um + CQ_OFF + (ci & ((1u << LOG_CQ) - 1)) * 64;
      uint8_t op_own = ((volatile uint8_t *)cqe)[63]; polls++;
      if ((op_own >> 4) == 0xF || (op_own & 1) != ((ci >> LOG_CQ) & 1)) return;
      _mm_lfence();
      uint16_t ctr = (cqe[60] << 8) | cqe[61];
      if ((op_own >> 4) == 2) seen.push_back(word_sum(base + DATA_OFF + (ctr & ((1u << LOG_RQ) - 1)) * STRIDE));
      else { errs++; seen.push_back(0xEEEE0000u | op_own); }
      ci++;
      *(volatile uint32_t *)(um + CQ_DBR) = htobe32(ci & 0xffffff);
      *(volatile uint32_t *)(um + RQ_DBR) = htobe32((ci + (1u << LOG_RQ)) & 0xffff);
      _mm_sfence();
    }
  };
  while (sent < n) {
    unsigned slot = sent % 64;
    make_frame(tx + slot * FRAME, sent); expect[sent] = word_sum(tx + slot * FRAME);
    ibv_sge s{(uint64_t)(tx + slot * FRAME), (uint32_t)FRAME, tmr->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = sent; w.sg_list = &s; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(tq, &w, &bad)) { perror("post_send"); break; }
    sent++; inflight++;
    ibv_wc wc[16]; int k;
    do { k = ibv_poll_cq(tcq, 16, wc); inflight -= k > 0 ? k : 0; } while (inflight >= 60);
    if (hostpoll) hostpoll_step();
    if (gap) usleep(gap);
  }
  for (int t = 0; t < 2000 && inflight; t++) { ibv_wc wc[16]; int k = ibv_poll_cq(tcq, 16, wc); inflight -= k > 0 ? k : 0; usleep(500); }

  unsigned got; std::vector<uint32_t> ids;
  if (!hostpoll) { *(volatile uint32_t *)(base + STOP_OFF) = 1; _mm_sfence(); }   // done sending
  if (hostpoll) {
    for (int t = 0; t < 2000 && seen.size() < n; t++) { hostpoll_step(); usleep(500); }
    got = seen.size();
  } else {
    auto st = run.wait(std::chrono::milliseconds(20000));
    if (st != ERT_CMD_STATE_COMPLETED) printf("kernel did not finish (state %d) -- reading partial results\n", (int)st);
    res.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    std::vector<uint32_t> r(4 + 2 * n); res.read(r.data(), (4 + 2 * n) * 4, 0);
    // v2: r[0..3] = got, polls, errs, last; then (sum, id) pairs.  v3: 64-bit words (lo, hi) -- the same order.
    got = r[0]; polls = r[1]; errs = r[2];
    printf("kernel: frames %u, polls %u, error CQEs %u, last byte_cnt %u\n", r[0], r[1], r[2], r[3]);
    for (unsigned i = 0; i < got && i < n; i++) { seen.push_back(r[4 + 2 * i]); ids.push_back(r[5 + 2 * i]); }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  unsigned good = 0;
  for (unsigned i = 0; i < seen.size(); i++) {           // ids are 16-bit; n is capped at 65535
    unsigned id = ids.empty() ? i : ids[i];
    good += id < n && seen[i] == expect[id];
  }
  printf("sent %u (%.0f frames/s offered), consumed %u, dropped %u, error CQEs %u, consumed frames byte-correct %u/%u  (%.2f s)\n",
         sent, sent / secs, got, sent - got, errs, good, got, secs);
  bool pass = got == n && !errs && good == n;
  bool clean_overload = got < n && !errs && good == got;
  if (clean_overload) printf("overload: every consumed frame is correct; the %u missing frames were dropped for lack of posted RQ slots\n", sent - got);
  printf("RESULT %s (%s)\n", pass ? "PASS" : "FAIL",
         host ? "host-memory control" : hostpoll ? "E2a: rings in FPGA DDR, CPU consumer" : "E2: rings in FPGA DDR, RP kernel consumer");

  ibv_destroy_qp(tq); ibv_destroy_cq(tcq); ibv_dereg_mr(tmr); ibv_dealloc_pd(pt);
  e2_rx_destroy(&rx); mlx5dv_devx_free_uar(uar); mlx5dv_devx_umem_dereg(umem); ibv_dereg_mr(dmr); ibv_dealloc_pd(pd);
  ibv_close_device(cr); ibv_close_device(ct);
  return pass ? 0 : 1;
}
