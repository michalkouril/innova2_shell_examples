// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 echo (2026-09-29, E3 dev shell): the e3_echo RP kernel receives frames from ConnectX-5 port 0 into FPGA DDR and
// transmits them back out of port 0 (zero copy, header rewritten: EtherType 0x88B6 -> 0x88B8, source MAC ..:EC), ringing
// the ConnectX doorbell itself through the PEER port. No CPU in either direction of the data path.
// The host: setup (DevX RQ/CQ/TIR/steering + SQ/CQ, all rings in FPGA DDR; peer window -> UAR), then on port 1 (cable
// loop) it generates the traffic and checks every echoed frame byte for byte.
//   usage (root): e3_echo_host <user BDF> <e3 xclbin> <port0 dev> <port1 dev> <cx5 BDF of port0> [frames] [gap_us]
//                 [--uram]      rings + doorbell records in the bottom 1 MB of bank0 (on-chip URAM in DDR mode 'hybrid')
//                 [--pingpong]  one frame in flight at a time; report the echo round-trip latency
//                 [--v2 [--bf] [--inline N]]  e3_echo2 kernel: BlueFlame (WQE by MMIO), inline frames <= N bytes
//                 [--v5 [--fence]]  e3_echo5: e3_echo4 + kernel timestamp log; with --pingpong prints the E5.0 breakdown
//                 [--v4 [--fence]]  e3_echo4: e3_echo3 + a real read-back fence; --poison works with BlueFlame off too
//                 [--v3 [--poison]]  e3_echo3: one 64 B BlueFlame write per WQEBB; --poison corrupts the ring copy (BF proof)
//                 [--len L]     frame length (64..1024)
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include "devx_rx.h"
#include "devx_sq.h"
#include "devx_cq.h"

static const size_t BUF = 8 << 20, STRIDE = 2048, RXLEN = 1024;
static size_t FRAME = 1024;                               // --len
static const int LOG_RQ = 10, LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11;
static size_t DATA_OFF = 0, UMEM_OFF = 0x200000, STOP_OFF = 0x300000;   // --uram: rings at 0, frames at 1 MB
static const size_t UMEM_LEN = 0x60000;
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const size_t SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const uint16_t ET_IN = 0x88B6, ET_OUT = 0x88B8;
static const size_t PW_OFF = 0x100000;

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
static uint64_t bar_start(const std::string &bdf, int bar)
{
  std::ifstream f("/sys/bus/pci/devices/" + bdf + "/resource"); std::string l; uint64_t s = 0;
  for (int i = 0; i <= bar && std::getline(f, l); i++) s = strtoull(l.c_str(), nullptr, 16);
  return s;
}
static void make_frame(uint8_t *f, unsigned i)
{
  memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF;
  for (size_t k = 14; k < FRAME; k++) f[k] = (uint8_t)(k * 13 + i * 5);
  f[14] = 0xEE; f[15] = i & 0xFF; f[16] = (i >> 8) & 0xFF;
}
static void expect_echo(uint8_t *f, unsigned i) { make_frame(f, i); f[6] = 0x02; f[11] = 0xEC; f[12] = ET_OUT >> 8; f[13] = ET_OUT & 0xFF; }
// CQ with hardware completion timestamps (the ConnectX clock, shared by both ports)
static ibv_cq_ex *cq_ts(ibv_context *c, int n)
{
  ibv_cq_init_attr_ex a{}; a.cqe = n; a.wc_flags = IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
  ibv_cq_ex *q = ibv_create_cq_ex(c, &a); if (!q) { perror("create_cq_ex"); exit(1); } return q;
}
template <class F> static int poll_ts(ibv_cq_ex *q, F f)
{
  ibv_poll_cq_attr pa{}; if (ibv_start_poll(q, &pa)) return 0;
  int k = 0;
  do { f(q->wr_id, q->status, ibv_wc_read_byte_len(q), ibv_wc_read_completion_ts(q)); k++; } while (!ibv_next_poll(q));
  ibv_end_poll(q); return k;
}
static ibv_qp *raw_qp(ibv_pd *pd, ibv_cq *cq, int sw, int rw, bool rts)
{
  ibv_qp_init_attr ia{}; ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = sw; ia.cap.max_recv_wr = rw; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia); if (!qp) { perror("create_qp"); exit(1); }
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  if (rts) { a = {}; a.qp_state = IBV_QPS_RTS; ibv_modify_qp(qp, &a, IBV_QP_STATE); }
  return qp;
}

int main(int argc, char **argv)
{
  if (argc < 6) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF> [frames] [gap_us]\n", argv[0]); return 2; }
  unsigned n = argc > 6 ? atoi(argv[6]) : 1000, gap = argc > 7 ? atoi(argv[7]) : 20;
  if (n > 65535) n = 65535;
  char err[256];
  bool uram = false, pingpong = false, v2 = false, v3 = false, v4 = false, v5 = false, v6 = false; unsigned bf = 0, inl = 0, poison = 0, fence = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--uram")) uram = true; if (!strcmp(argv[i], "--pingpong")) pingpong = true;
    if (!strcmp(argv[i], "--v2")) v2 = true; if (!strcmp(argv[i], "--bf")) bf = 1;
    if (!strcmp(argv[i], "--v3")) v2 = v3 = true; if (!strcmp(argv[i], "--poison")) poison = 1;
    if (!strcmp(argv[i], "--v4")) v2 = v3 = v4 = true; if (!strcmp(argv[i], "--fence")) fence = 1;
    if (!strcmp(argv[i], "--v5")) v2 = v3 = v4 = v5 = true;
    if (!strcmp(argv[i], "--v6")) v2 = v3 = v4 = v5 = v6 = true;   // e3_echo6 + the e5_ticker kernel
    if (!strcmp(argv[i], "--inline") && i + 1 < argc) inl = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--len") && i + 1 < argc) FRAME = atoi(argv[i + 1]);
  }
  if (FRAME < 64 || FRAME > 1024) { fprintf(stderr, "--len must be 64..1024\n"); return 2; }
  if (inl > 222) inl = 222;                                   // a WQE must fit one 256-byte BlueFlame buffer
  if (uram) { UMEM_OFF = 0x0; DATA_OFF = 0x100000; STOP_OFF = 0x380000; }

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krnl(dev, uuid, v6 ? "e3_echo6" : v5 ? "e3_echo5" : v4 ? "e3_echo4" : v3 ? "e3_echo3" : v2 ? "e3_echo2" : "e3_echo");
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, krnl.group_id(0));
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, krnl.group_id(1));
  xrt::bo res(dev, 4096, xrt::bo::flags::normal, krnl.group_id(2));
  xrt::bo tlog = v5 ? xrt::bo(dev, 4096 * 64, xrt::bo::flags::normal, krnl.group_id(30)) : xrt::bo();   // e3_echo5 timestamp log
  uint8_t *base = bo.map<uint8_t *>(), *um = base + UMEM_OFF;
  if (uram) {
    printf("--uram: rings at bank0 offset 0x%llx..0x%llx (%s)\n", (unsigned long long)(bo.address() - 0x200000000ull),
           (unsigned long long)(bo.address() - 0x200000000ull + UMEM_LEN),
           bo.address() == 0x200000000ull ? "inside the 1 MB URAM window" : "*** NOT at bank0 offset 0 -- not in URAM");
    if (bo.address() != 0x200000000ull) return 1;
  }

  ibv_context *c0 = open_dev(argv[3], true), *c1 = open_dev(argv[4], false);
  if (!c0 || !c1) { fprintf(stderr, "cannot open devices\n"); return 1; }
  ibv_pd *pd = ibv_alloc_pd(c0);
  ibv_mr *dmr = ibv_reg_mr(pd, base + DATA_OFF, (1u << LOG_RQ) * STRIDE, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *umem = mlx5dv_devx_umem_reg(c0, um, UMEM_LEN, IBV_ACCESS_LOCAL_WRITE);
  if (!dmr || !umem) { printf("registration failed: %s (is innova2_peermem loaded?)\n", strerror(errno)); return 1; }
  mlx5dv_pd dpd{}; mlx5dv_obj o{}; o.pd.in = pd; o.pd.out = &dpd; mlx5dv_init_obj(&o, MLX5DV_OBJ_PD);
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(c0, MLX5DV_UAR_ALLOC_TYPE_NC);
  memset(um, 0, UMEM_LEN);
  memset(base + STOP_OFF, 0, 64);
  for (unsigned i = 0; i < (1u << LOG_RCQ); i++) um[RCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_TCQ); i++) um[TCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_RQ); i++)
    mlx5dv_set_data_seg((mlx5_wqe_data_seg *)(um + RQ_OFF + i * 16), STRIDE, dmr->lkey, (uintptr_t)(base + DATA_OFF + i * STRIDE));
  _mm_sfence();

  // RX (port 0 -> FPGA DDR) and TX (FPGA DDR -> port 0), all rings in the umem
  e2_rx_cfg rcfg{dpd.pdn, uar->page_id, umem->umem_id, RCQ_OFF, RCQ_DBR, RQ_OFF, RQ_DBR, LOG_RCQ, LOG_RQ, ET_IN};
  e2_rx rx;
  if (e2_rx_create(c0, &rcfg, &rx, err, sizeof(err))) { printf("*** RX: %s\n", err); return 1; }
  *(volatile uint32_t *)(um + RQ_DBR) = htobe32(1u << LOG_RQ);
  e3_cq tcq; e1_sq sq;
  if (e3_cq_create(c0, uar->page_id, umem->umem_id, TCQ_OFF, TCQ_DBR, LOG_TCQ, &tcq, err, sizeof(err))) { printf("*** TX CQ: %s\n", err); return 1; }
  int mi = e1_query_min_inline(c0, err, sizeof(err)); if (mi < 0) mi = 1;
  if (e1_create_sq(c0, dpd.pdn, tcq.cqn, uar->page_id, umem->umem_id, SQ_OFF, umem->umem_id, SQ_DBR, LOG_SQ, mi, &sq, err, sizeof(err))) { printf("*** SQ: %s\n", err); return 1; }
  _mm_sfence();

  const uint64_t uar_bus = bar_start(argv[5], 0) + (uint64_t)uar->page_id * 4096 + ((uint8_t *)uar->reg_addr - (uint8_t *)uar->base_addr);
  const unsigned db_off = (unsigned)((uar_bus & 0xFFFFF) - (pbo.address() & 0xFFFFF));
  int fd = open(("/sys/bus/pci/devices/" + std::string(argv[1]) + "/resource0").c_str(), O_RDWR | O_SYNC);
  volatile uint32_t *pw = (volatile uint32_t *)((uint8_t *)mmap(nullptr, 1 << 25, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) + PW_OFF);
  if (pw[0] != 0x50454552) { printf("*** peer_window not found -- is the E3 shell running?\n"); return 1; }
  pw[1] = 0; pw[2] = (uint32_t)(uar_bus & ~0xFFFFFull); pw[3] = (uint32_t)(uar_bus >> 32);
  const uint32_t w0 = pw[5]; pw[1] = 1;
  printf("E3 echo: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | doorbell bus 0x%llx via PEER offset 0x%x\n",
         rx.rqn, rx.cqn, sq.sqn, tcq.cqn, (unsigned long long)uar_bus, db_off);

  // the kernel's argument list; e3_echo2 appends (bf, inline_max, bf_size = 256: the CX5's 512 B BlueFlame register holds
  // two 256 B buffers, used alternately)
  xrt::run run(krnl);
  {
    int a = 0;
    run.set_arg(a++, bo); run.set_arg(a++, pbo); run.set_arg(a++, res);
    for (unsigned v : {(unsigned)(UMEM_OFF + RQ_DBR), (unsigned)LOG_RQ, (unsigned)(UMEM_OFF + RCQ_OFF), (unsigned)(UMEM_OFF + RCQ_DBR),
                       (unsigned)LOG_RCQ, (unsigned)DATA_OFF, (unsigned)STRIDE}) run.set_arg(a++, v);
    run.set_arg(a++, (unsigned long long)(uintptr_t)(base + DATA_OFF));
    for (unsigned v : {(unsigned)(UMEM_OFF + SQ_OFF), (unsigned)LOG_SQ, (unsigned)(UMEM_OFF + SQ_DBR), (unsigned)(UMEM_OFF + TCQ_OFF),
                       (unsigned)(UMEM_OFF + TCQ_DBR), (unsigned)LOG_TCQ, sq.sqn, dmr->lkey, db_off, (unsigned)ET_OUT, n,
                       400000000u, (unsigned)STOP_OFF}) run.set_arg(a++, v);
    if (v2) { run.set_arg(a++, bf); run.set_arg(a++, inl); run.set_arg(a++, 256u); }
    if (v3) { run.set_arg(a++, poison); run.set_arg(a++, pbo); }   // peerw: the same PEER BO, 64-byte BlueFlame writes
    if (v4) run.set_arg(a++, fence);
    if (v5) run.set_arg(a++, tlog);
    run.start();
  }
  if (v2) printf("%s: BlueFlame %s%s, inline frames <= %u B, frame length %zu B\n", v6 ? (fence ? "e3_echo6 [fence]" : "e3_echo6") : v5 ? (fence ? "e3_echo5 [fence]" : "e3_echo5") : v4 ? (fence ? "e3_echo4 [fence]" : "e3_echo4") : v3 ? "e3_echo3" : "e3_echo2", bf ? "on" : "off", poison ? " (ring copy POISONED)" : "", inl, FRAME);

  // port 1: traffic generator + echo checker (host memory)
  ibv_pd *p1 = ibv_alloc_pd(c1);
  uint8_t *tx = (uint8_t *)aligned_alloc(4096, 64 * RXLEN), *rxb = (uint8_t *)aligned_alloc(4096, 1024 * RXLEN);
  ibv_mr *tmr = ibv_reg_mr(p1, tx, 64 * RXLEN, 0), *rmr = ibv_reg_mr(p1, rxb, 1024 * RXLEN, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq_ex *scqx = cq_ts(c1, 128), *ecqx = cq_ts(c1, 2048);
  ibv_cq *scq = ibv_cq_ex_to_cq(scqx), *ecq = ibv_cq_ex_to_cq(ecqx);
  ibv_qp *sqp = raw_qp(p1, scq, 64, 1, true), *eqp = raw_qp(p1, ecq, 1, 1024, false);
  std::vector<uint64_t> ts_tx1(n, 0), ts_rx1(n, 0);           // port-1 send / receive completion timestamps per frame
  struct { ibv_flow_attr a; ibv_flow_spec_eth e; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 1; fl.a.port = 1;
  fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(ET_OUT); fl.e.mask.ether_type = 0xFFFF;
  ibv_flow *flow = ibv_create_flow(eqp, &fl.a);
  if (!flow) { perror("create_flow"); return 1; }
  auto post_rx = [&](unsigned slot) {
    ibv_sge s{(uint64_t)(rxb + slot * RXLEN), (uint32_t)RXLEN, rmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = slot; w.sg_list = &s; w.num_sge = 1;
    ibv_post_recv(eqp, &w, &bad);
  };
  for (unsigned i = 0; i < 1024; i++) post_rx(i);

  unsigned sent = 0, inflight = 0, got = 0, good = 0, rxerr = 0;
  std::vector<uint8_t> ref(FRAME);
  auto drain = [&]() {
    while (poll_ts(ecqx, [&](uint64_t wr, ibv_wc_status stt, uint32_t len, uint64_t t) {
        unsigned slot = (unsigned)wr;
        if (stt) rxerr++;
        else { unsigned id = rxb[slot * RXLEN + 15] | (rxb[slot * RXLEN + 16] << 8); expect_echo(ref.data(), id);
               good += len == FRAME && !memcmp(rxb + slot * RXLEN, ref.data(), FRAME); if (id < n) ts_rx1[id] = t; }
        got++; post_rx(slot);
      }) > 0) {}
  };
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> rtt;
  while (sent < n) {
    auto ts = std::chrono::steady_clock::now();
    unsigned s = sent % 64; make_frame(tx + s * RXLEN, sent);
    ibv_sge sg{(uint64_t)(tx + s * RXLEN), (uint32_t)FRAME, tmr->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = sent; w.sg_list = &sg; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(sqp, &w, &bad)) { perror("post_send"); break; }
    sent++; inflight++;
    int k; do { k = poll_ts(scqx, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { if (wr < n) ts_tx1[wr] = t; }); inflight -= k; } while (inflight >= 60);
    drain();
    if (pingpong) {                                     // wait for this frame's echo before the next one
      while (got < sent && std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count() < 0.2) drain();
      if (got == sent) rtt.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - ts).count());
    }
    if (gap) usleep(gap);
  }
  for (int t = 0; t < 3000 && got < n; t++) { drain(); usleep(1000); }
  *(volatile uint32_t *)(base + STOP_OFF) = 1; _mm_sfence();
  auto st = run.wait(std::chrono::milliseconds(20000));
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  res.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  uint32_t r[8]; res.read(r, sizeof(r), 0);
  printf("kernel (%s): received %u, WQEs %u, TX CQEs %u, RX errors %u, TX errors %u, doorbells %u, polls %u\n",
         st == ERT_CMD_STATE_COMPLETED ? "done" : "NOT done", r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
  if (r[4]) printf("first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x\n", r[7] >> 28, (r[7] >> 16) & 0xFF, (r[7] >> 8) & 0xFF);
  printf("peer window: doorbell writes forwarded %u\n", pw[5] - w0);
  printf("port 1: sent %u, echoes received %u, byte-exact %u, errors %u  (%.3f s, %.0f echoes/s)\n", sent, got, good, rxerr, secs, got / secs);
  if (pingpong && !rtt.empty()) {
    std::vector<double> v = rtt; std::sort(v.begin(), v.end()); double m = 0; for (double x : v) m += x; m /= v.size();
    printf("round trip (host send -> FPGA echo -> host receive), %zu samples: mean %.2f us, p50 %.2f, p99 %.2f, min %.2f  [%s rings]\n",
           v.size(), m, v[v.size() / 2], v[v.size() * 99 / 100], v[0], uram ? "URAM" : "DDR");
  }
  if (pingpong && n <= (1u << LOG_RCQ) && n <= (1u << LOG_TCQ)) {
    // per-frame breakdown on the ConnectX clock: port-1 TX -> port-0 RX (FPGA RX CQE) -> port-0 TX (FPGA TX CQE) -> port-1 RX
    ibv_device_attr_ex da{}; ibv_query_device_ex(c1, nullptr, &da);
    const double us = 1000.0 / (double)da.hca_core_clock;   // hca_core_clock is in kHz
    auto cqe_ts = [&](size_t ring, unsigned i) { uint64_t v; memcpy(&v, um + ring + (size_t)i * 64 + 48, 8); return be64toh(v) & 0x7fffffffffffffffull; };
    std::vector<double> wo, fp, wb, nic, host;
    for (unsigned i = 0; i < n && i < rtt.size(); i++) {
      uint64_t a = ts_tx1[i], b = cqe_ts(RCQ_OFF, i), c = cqe_ts(TCQ_OFF, i), d = ts_rx1[i];
      if (!a || !b || !c || !d || !(a <= b && b <= c && c <= d)) continue;
      wo.push_back((b - a) * us); fp.push_back((c - b) * us); wb.push_back((d - c) * us); nic.push_back((d - a) * us);
      host.push_back(rtt[i] - (d - a) * us);
    }
    auto st3 = [](std::vector<double> v, const char *name) {
      if (v.empty()) { printf("  %-38s (no samples)\n", name); return; }
      std::sort(v.begin(), v.end()); double m = 0; for (double x : v) m += x; m /= v.size();
      printf("  %-38s mean %6.2f us  p50 %6.2f  p99 %6.2f\n", name, m, v[v.size() / 2], v[v.size() * 99 / 100]);
    };
    printf("breakdown (%zu of %zu frames with all four ConnectX timestamps; clock %.2f MHz):\n", fp.size(), rtt.size(), da.hca_core_clock / 1000.0);
    st3(host, "host: post_send..TX + RX CQE..seen");
    st3(wo,   "wire out: port1 TX -> port0 RX");
    st3(fp,   "FPGA echo: port0 RX CQE -> TX CQE");
    st3(wb,   "wire back: port0 TX -> port1 RX");
    st3(nic,  "NIC total: port1 TX -> port1 RX");
    if (v5) {
      // E5.0: the kernel's own timestamps (t0 RX CQE seen .. t4 TX CQE seen), aligned with the ConnectX clock
      tlog.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
      std::vector<uint64_t> L((size_t)n * 8); tlog.read(L.data(), L.size() * 8, 0);
      std::vector<unsigned> ok; std::vector<double> bx, ky;
      for (unsigned i = 0; i < n && i < 4096; i++) {
        const uint64_t *e = &L[(size_t)i * 8];
        uint64_t b = cqe_ts(RCQ_OFF, i), c = cqe_ts(TCQ_OFF, i);
        if (e[7] != i || !b || !c || c < b || !(e[0] <= e[1] && e[1] <= e[2] && e[2] <= e[3] && e[3] <= e[4])) continue;
        ok.push_back(i); bx.push_back((double)b); ky.push_back((double)e[0]);
      }
      if (ok.size() < 10) { printf("E5.0: too few usable records (%zu)\n", ok.size()); }
      else {
        // kernel clock: least-squares slope of t0 (kernel ticks) against the RX CQE timestamp (NIC ticks)
        double mb = 0, mk = 0; for (size_t j = 0; j < ok.size(); j++) { mb += bx[j]; mk += ky[j]; } mb /= ok.size(); mk /= ok.size();
        double sxy = 0, sxx = 0; for (size_t j = 0; j < ok.size(); j++) { sxy += (bx[j] - mb) * (ky[j] - mk); sxx += (bx[j] - mb) * (bx[j] - mb); }
        const double kmhz = sxy / sxx * da.hca_core_clock / 1000.0, kus = 1.0 / kmhz;
        std::vector<double> dread, dbuild, dring, dwork, dtxwait, rxlag, txlag, fixed, pollrx, polltx;
        double base = 1e300;
        for (unsigned i : ok) { const uint64_t *e = &L[(size_t)i * 8]; base = std::min(base, e[0] * kus - cqe_ts(RCQ_OFF, i) * us); }
        for (unsigned i : ok) {
          const uint64_t *e = &L[(size_t)i * 8];
          const double b = cqe_ts(RCQ_OFF, i) * us, c = cqe_ts(TCQ_OFF, i) * us;
          dread.push_back((e[1] - e[0]) * kus); dbuild.push_back((e[2] - e[1]) * kus); dring.push_back((e[3] - e[2]) * kus);
          dwork.push_back((e[3] - e[0]) * kus); dtxwait.push_back((e[4] - e[3]) * kus);
          const double rl = e[0] * kus - b - base, tl = e[4] * kus - c - base;   // lags past the fastest frame's RX lag
          rxlag.push_back(rl); txlag.push_back(tl);
          fixed.push_back((c - b) - (e[3] - e[0]) * kus - rl);                   // echo not explained by kernel work or RX lag
          if (e[6] > e[5] && e[4] > e[0]) polltx.push_back((e[4] - e[0]) * kus / (double)(e[6] - e[5]));
        }
        printf("E5.0 kernel timestamps: %zu frames, kernel clock %.2f MHz (fitted against the ConnectX clock)\n", ok.size(), kmhz);
        st3(dread,  "k: RX CQE seen -> frame read back");
        st3(dbuild, fence ? "k: frame -> WQE+DBR written (+fence)" : "k: frame -> WQE+DBR issued");
        st3(dring,  "k: -> doorbell/BlueFlame issued");
        st3(dwork,  "k: kernel work (RX CQE seen -> ring)");
        st3(dtxwait,"k: ring -> TX CQE seen");
        st3(rxlag,  "RX detection lag beyond fastest frame");
        st3(txlag,  "TX CQE seen - TX CQE ts (same base)");
        st3(fixed,  "echo - kernel work - RX lag excess");
        st3(polltx, "poll loop iteration (while waiting TX)");
      }
    }
  }
  bool pass = got == n && good == n && !rxerr && r[0] == n && r[2] == n && !r[3] && !r[4] && pw[5] - w0 == (bf ? r[1] * (v3 ? 1 : 8) : r[5]);   // BlueFlame: 8 qword writes (v2) or one 64 B write (v3) per WQEBB
  printf("RESULT %s (E3 echo: RX and TX through the ConnectX with no CPU in the data path)\n", pass ? "PASS" : "FAIL");
  pw[1] = 0;
  ibv_destroy_flow(flow); ibv_destroy_qp(eqp); ibv_destroy_qp(sqp); ibv_destroy_cq(ecq); ibv_destroy_cq(scq);
  ibv_dereg_mr(rmr); ibv_dereg_mr(tmr); ibv_dealloc_pd(p1);
  e1_destroy_sq(&sq); e3_cq_destroy(&tcq); e2_rx_destroy(&rx); mlx5dv_devx_free_uar(uar); mlx5dv_devx_umem_dereg(umem);
  ibv_dereg_mr(dmr); ibv_dealloc_pd(pd); ibv_close_device(c0); ibv_close_device(c1);
  return pass ? 0 : 1;
}
