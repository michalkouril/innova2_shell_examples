// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.1 host (2026-09-30): sets up the ConnectX queues exactly as E3 (DevX RQ/RX CQ/TIR/steering + SQ/TX CQ, rings in the
// bank0 URAM window, frames in DDR, doorbells through the PEER window), starts the FLD-lite engines (fld_rx, fld_tx,
// fld_reap; afu_loop is free-running) and drives traffic from port 1 through the loopback cable.
//   usage (root): e5_host <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF of port0> [frames] [options]
//     --lat          ping-pong: one frame in flight; ConnectX-timestamp breakdown (forces --sig 1)
//     --burst        unpaced: up to 60 frames in flight on the generator; counts drops, measures frames/s
//     --len L        frame length 60..1500 (default 1024); --mix cycles 60..1500
//     --bf           BlueFlame (else 8-byte doorbells); --inline N  inline frames <= N bytes (max 222)
//     --sig K        CQ_UPDATE on every K-th WQE (default 16; 1 in --lat)
//     --stamp        E5.1b: kernel timestamps (e5_ticker2); with --lat prints the per-stage breakdown
//     --rxddr        RX frame slots in DDR (1024 x 2 KB at 1 MB) instead of the URAM window (256 x 2 KB at 0x80000)
//     --pre N        fld_rx reads the first N (0..4, default 2) frame beats with each CQE poll
//     --gap N        fld_reap idle back-off cycles (default 64); --maxb N  fld_tx batch limit (default 16, 1 = off)
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include "devx_rx.h"
#include "devx_sq.h"
#include "devx_cq.h"

static const size_t BUF = 8 << 20, STRIDE = 2048, RXLEN = 2048;
static int LOG_RQ = 8; static const int LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11, LOG_TXS = 10;
static size_t DATA_OFF = 0x80000;                         // RX slots in the URAM window (E5.1c default)
static const size_t UMEM_OFF = 0x0, UMEM_LEN = 0x60000, TXB_OFF = 0x400000, STOP_OFF = 0x380000;
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const size_t SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const uint16_t ET_IN = 0x88B6, ET_OUT = 0x88B8;
static const size_t PW_OFF = 0x100000;
static unsigned FRAME = 1024;
static bool MIX = false;

static unsigned flen(unsigned i) { static const unsigned L[] = {60, 64, 100, 128, 200, 222, 223, 256, 511, 1000, 1500, 777}; return MIX ? L[i % 12] : FRAME; }
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
  const unsigned len = flen(i);
  memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF;
  for (size_t k = 14; k < len; k++) f[k] = (uint8_t)(k * 13 + i * 5 + (k >> 8));
  f[14] = 0xEE; f[15] = i & 0xFF; f[16] = (i >> 8) & 0xFF; f[17] = (i >> 16) & 0xFF;
}
static void expect_echo(uint8_t *f, unsigned i) { make_frame(f, i); f[6] = 0x02; f[11] = 0xEC; f[12] = ET_OUT >> 8; f[13] = ET_OUT & 0xFF; }
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
static void st3(std::vector<double> v, const char *name)
{
  if (v.empty()) { printf("  %-40s (no samples)\n", name); return; }
  std::sort(v.begin(), v.end()); double m = 0; for (double x : v) m += x; m /= v.size();
  printf("  %-40s mean %6.2f us  p50 %6.2f  p99 %6.2f\n", name, m, v[v.size() / 2], v[v.size() * 99 / 100]);
}

int main(int argc, char **argv)
{
  if (argc < 6) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF> [frames] [options]\n", argv[0]); return 2; }
  unsigned n = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 1000;
  bool lat = false, burst = false; unsigned bf = 0, inl = 0, sig = 16, stamp = 0, spec = 2, gap = 64, maxb = 16;
  for (int i = 6; i < argc; i++) {
    if (!strcmp(argv[i], "--lat")) lat = true; if (!strcmp(argv[i], "--burst")) burst = true;
    if (!strcmp(argv[i], "--bf")) bf = 1; if (!strcmp(argv[i], "--mix")) MIX = true;
    if (!strcmp(argv[i], "--inline") && i + 1 < argc) inl = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--inline-adapt")) inl |= 1u << 16;          // E5.5: inline only lone frames on an idle link
    if (!strcmp(argv[i], "--len") && i + 1 < argc) FRAME = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--sig") && i + 1 < argc) sig = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--stamp")) stamp = 1;
    if (!strcmp(argv[i], "--rxddr")) { DATA_OFF = 0x100000; LOG_RQ = 10; }
    if (!strcmp(argv[i], "--pre") && i + 1 < argc) spec = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--maxb") && i + 1 < argc) maxb = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atoi(argv[i + 1]);
  }
  if (FRAME < 60 || FRAME > 1500) { fprintf(stderr, "--len must be 60..1500\n"); return 2; }
  if ((inl & 0xffff) > 222) inl = (inl & ~0xffffu) | 222;       // clamp the size only: bit 16 = E5.5 adaptive inline
  if (lat) sig = 1;
  if (sig < 1) sig = 1;
  if (n > 1000000) n = 1000000;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krx(dev, uuid, "fld_rx"), ktx(dev, uuid, "fld_tx"), krp(dev, uuid, "fld_reap");
  // the ring/frame BO first, so it lands at bank0 offset 0 (the HYBRID URAM window)
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, krx.group_id(0));
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, ktx.group_id(2));
  xrt::bo rres(dev, 4096, xrt::bo::flags::normal, krx.group_id(2)), tres(dev, 4096, xrt::bo::flags::normal, ktx.group_id(3)),
          qres(dev, 4096, xrt::bo::flags::normal, krp.group_id(1));
  xrt::bo rlog(dev, 4096 * 64, xrt::bo::flags::normal, krx.group_id(15)), tlog(dev, 4096 * 64, xrt::bo::flags::normal, ktx.group_id(22));
  uint8_t *base = bo.map<uint8_t *>(), *um = base + UMEM_OFF;
  if (bo.address() != 0x200000000ull) { printf("*** ring BO at 0x%llx, not bank0 offset 0 (URAM window)\n", (unsigned long long)bo.address()); return 1; }

  ibv_context *c0 = open_dev(argv[3], true), *c1 = open_dev(argv[4], false);
  if (!c0 || !c1) { fprintf(stderr, "cannot open devices\n"); return 1; }
  ibv_pd *pd = ibv_alloc_pd(c0);
  // one MR over RX slots, the stop word and the TX buffer slots
  ibv_mr *dmr = ibv_reg_mr(pd, base + DATA_OFF, TXB_OFF + (STRIDE << LOG_TXS) - DATA_OFF, IBV_ACCESS_LOCAL_WRITE);
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
  printf("E5.1 FLD-lite: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%llx via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, %s\n",
         rx.rqn, rx.cqn, sq.sqn, tcq.cqn, (unsigned long long)uar_bus, db_off, bf ? "on" : "off", inl, sig,
         MIX ? "lengths 60..1500 mixed" : (std::to_string(FRAME) + " B frames").c_str());

  // engines: stream args (fld_rx 3; fld_tx 4,5; fld_reap 2) are not set by the host
  xrt::run rrx(krx), rtx(ktx), rrp(krp);
  rrx.set_arg(0, bo); rrx.set_arg(1, bo); rrx.set_arg(2, rres);
  { int a = 4; for (unsigned v : {(unsigned)(UMEM_OFF + RCQ_OFF), (unsigned)LOG_RCQ, (unsigned)(UMEM_OFF + RCQ_DBR), (unsigned)(UMEM_OFF + RQ_DBR),
                                  (unsigned)LOG_RQ, (unsigned)DATA_OFF, (unsigned)STRIDE, (unsigned)STOP_OFF, 16u, spec, stamp}) rrx.set_arg(a++, v);
    rrx.set_arg(15, rlog); }
  rtx.set_arg(0, bo); rtx.set_arg(1, bo); rtx.set_arg(2, pbo); rtx.set_arg(3, tres);
  { int a = 6;
    for (unsigned v : {(unsigned)(UMEM_OFF + SQ_OFF), (unsigned)LOG_SQ, (unsigned)(UMEM_OFF + SQ_DBR), sq.sqn, dmr->lkey, (unsigned)TXB_OFF}) rtx.set_arg(a++, v);
    rtx.set_arg(a++, (unsigned long long)(uintptr_t)(base + TXB_OFF));
    for (unsigned v : {(unsigned)LOG_TXS, (unsigned)STRIDE, db_off, bf, 256u, inl, sig, (unsigned)STOP_OFF, stamp}) rtx.set_arg(a++, v);
    rtx.set_arg(22, tlog); rtx.set_arg(25, maxb); }
  rrp.set_arg(0, bo); rrp.set_arg(1, qres);
  { int a = 3; for (unsigned v : {(unsigned)(UMEM_OFF + TCQ_OFF), (unsigned)LOG_TCQ, (unsigned)(UMEM_OFF + TCQ_DBR), (unsigned)STOP_OFF, gap}) rrp.set_arg(a++, v); }
  rrp.start(); rtx.start(); rrx.start();

  // port 1: traffic generator + echo checker (host memory)
  ibv_pd *p1 = ibv_alloc_pd(c1);
  uint8_t *tx = (uint8_t *)aligned_alloc(4096, 64 * RXLEN), *rxb = (uint8_t *)aligned_alloc(4096, 1024 * RXLEN);
  ibv_mr *tmr = ibv_reg_mr(p1, tx, 64 * RXLEN, 0), *rmr = ibv_reg_mr(p1, rxb, 1024 * RXLEN, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq_ex *scqx = cq_ts(c1, 128), *ecqx = cq_ts(c1, 2048);
  ibv_cq *scq = ibv_cq_ex_to_cq(scqx), *ecq = ibv_cq_ex_to_cq(ecqx);
  ibv_qp *sqp = raw_qp(p1, scq, 64, 1, true), *eqp = raw_qp(p1, ecq, 1, 1024, false);
  std::vector<uint64_t> ts_tx1(n, 0), ts_rx1(n, 0);
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

  unsigned sent = 0, inflight = 0, got = 0, good = 0, rxerr = 0, ooo = 0, next_id = 0;
  std::vector<uint8_t> ref(2048);
  auto drain = [&]() {
    while (poll_ts(ecqx, [&](uint64_t wr, ibv_wc_status stt, uint32_t len, uint64_t t) {
        unsigned slot = (unsigned)wr;
        if (stt) rxerr++;
        else {
          const uint8_t *f = rxb + slot * RXLEN;
          unsigned id = f[15] | (f[16] << 8) | (f[17] << 16);
          if (id < n) { expect_echo(ref.data(), id); good += len == flen(id) && !memcmp(f, ref.data(), flen(id)); ts_rx1[id] = t; }
          ooo += id != next_id; next_id = id + 1;
        }
        got++; post_rx(slot);
      }) > 0) {}
  };
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> rtt;
  while (sent < n) {
    auto ts = std::chrono::steady_clock::now();
    unsigned s = sent % 64; make_frame(tx + s * RXLEN, sent);
    ibv_sge sg{(uint64_t)(tx + s * RXLEN), flen(sent), tmr->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = sent; w.sg_list = &sg; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(sqp, &w, &bad)) { perror("post_send"); break; }
    sent++; inflight++;
    int k; do { k = poll_ts(scqx, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { if (wr < n) ts_tx1[wr] = t; }); inflight -= k; } while (inflight >= 60);
    drain();
    if (lat) {
      while (got < sent && std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count() < 0.2) drain();
      if (got == sent) rtt.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - ts).count());
    } else if (!burst) usleep(2);
  }
  for (int t = 0; t < 2000 && got < n; t++) { drain(); usleep(1000); }
  while (inflight) { int k = poll_ts(scqx, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { if (wr < n) ts_tx1[wr] = t; }); inflight -= k; if (!k) break; }
  *(volatile uint32_t *)(base + STOP_OFF) = 1; _mm_sfence();
  auto s1 = rrx.wait(std::chrono::milliseconds(10000)), s2 = rtx.wait(std::chrono::milliseconds(10000)), s3 = rrp.wait(std::chrono::milliseconds(10000));
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  rres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); tres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); qres.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  uint32_t r[8], t[8], q[8]; rres.read(r, sizeof(r), 0); tres.read(t, sizeof(t), 0); qres.read(q, sizeof(q), 0);
  auto dn = [](ert_cmd_state s) { return s == ERT_CMD_STATE_COMPLETED ? "done" : "NOT done"; };
  printf("fld_rx  (%s): CQEs %u, frames %u, RX errors %u, slot mismatches %u, polls %u\n", dn(s1), r[0], r[1], r[2], r[3], r[4]);
  printf("fld_tx  (%s): frames %u, WQEBBs %u, inline %u, buffered %u, credit waits %u, polls %u, batches %u\n", dn(s2), t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
  printf("fld_reap(%s): TX CQEs %u, error CQEs %u%s\n", dn(s3), q[0], q[1], q[1] ? "" : "");
  if (q[1]) printf("first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x\n", q[2] >> 28, (q[2] >> 16) & 0xFF, (q[2] >> 8) & 0xFF);
  printf("peer window: writes forwarded %u\n", pw[5] - w0);
  ibv_device_attr_ex da{}; ibv_query_device_ex(c1, nullptr, &da);
  const double us = 1000.0 / (double)da.hca_core_clock;
  printf("port 1: sent %u, echoes received %u, byte-exact %u, out of order %u, RX errors %u, lost %u  (%.3f s)\n",
         sent, got, good, ooo, rxerr, sent - got, secs);
  if (!lat && got > 1) {
    uint64_t a = ts_tx1[0], b = 0; for (unsigned i = 0; i < n; i++) b = std::max(b, ts_rx1[i]);
    if (a && b > a) {
      double d = (b - a) * us * 1e-6; double bytes = 0; for (unsigned i = 0; i < n; i++) bytes += flen(i);
      printf("rate (ConnectX clock, first send -> last echo): %.0f frames/s, %.2f Gb/s of frames each way\n", got / d, bytes * 8 / d * 1e-9);
    }
  }
  if (lat && !rtt.empty()) {
    auto cqe_ts = [&](size_t ring, unsigned i) { uint64_t v; memcpy(&v, um + ring + (size_t)i * 64 + 48, 8); return be64toh(v) & 0x7fffffffffffffffull; };
    std::vector<double> wo, fp, wb, host;
    for (unsigned i = 0; i < n && i < rtt.size() && i < (1u << LOG_RCQ) && i < (1u << LOG_TCQ); i++) {
      uint64_t a = ts_tx1[i], b = cqe_ts(RCQ_OFF, i), c = cqe_ts(TCQ_OFF, i), d = ts_rx1[i];
      if (!a || !b || !c || !d || !(a <= b && b <= c && c <= d)) continue;
      wo.push_back((b - a) * us); fp.push_back((c - b) * us); wb.push_back((d - c) * us); host.push_back(rtt[i] - (d - a) * us);
    }
    printf("breakdown (%zu frames, ConnectX clock):\n", fp.size());
    st3(rtt, "round trip (host send -> host receive)");
    st3(host, "host: post_send..TX + RX CQE..seen");
    st3(wo, "wire out: port1 TX -> port0 RX");
    st3(fp, "FPGA echo: port0 RX CQE -> TX CQE");
    st3(wb, "wire back: port0 TX -> port1 RX");
    if (stamp) {
      rlog.sync(XCL_BO_SYNC_BO_FROM_DEVICE); tlog.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
      std::vector<uint64_t> R(4096 * 8), T(4096 * 8); rlog.read(R.data(), R.size() * 8, 0); tlog.read(T.data(), T.size() * 8, 0);
      std::vector<unsigned> ok; double mb = 0, mk = 0;
      for (unsigned i = 0; i < n && i < 2048 && i < rtt.size(); i++) {
        const uint64_t *a = &R[i * 8], *b = &T[i * 8];
        if ((a[2] & 0xffffffff) != i || (b[4] & 0xffffffff) != i) continue;
        if (!(a[0] && a[0] <= a[1] && a[0] <= b[0] && b[0] <= b[1] && b[1] <= b[2] && b[2] <= b[3])) continue;   // tx may take beat 0 before rx stamps its last beat
        if (!cqe_ts(RCQ_OFF, i) || !cqe_ts(TCQ_OFF, i)) continue;
        ok.push_back(i); mb += cqe_ts(RCQ_OFF, i); mk += a[0];
      }
      if (getenv("E5_DUMP")) for (unsigned i = 0; i < 3; i++)
        printf("rlog[%u] %llx %llx %llx | tlog[%u] %llx %llx %llx %llx %llx | NIC b %llx c %llx\n", i,
               (unsigned long long)R[i * 8], (unsigned long long)R[i * 8 + 1], (unsigned long long)R[i * 8 + 2], i,
               (unsigned long long)T[i * 8], (unsigned long long)T[i * 8 + 1], (unsigned long long)T[i * 8 + 2],
               (unsigned long long)T[i * 8 + 3], (unsigned long long)T[i * 8 + 4],
               (unsigned long long)cqe_ts(RCQ_OFF, i), (unsigned long long)cqe_ts(TCQ_OFF, i));
      if (ok.size() < 10) printf("stamps: too few usable records (%zu)\n", ok.size());
      else {
        mb /= ok.size(); mk /= ok.size(); double sxy = 0, sxx = 0;
        for (unsigned i : ok) { double x = cqe_ts(RCQ_OFF, i) - mb; sxy += x * (R[i * 8] - mk); sxx += x * x; }
        const double kmhz = sxy / sxx * da.hca_core_clock / 1000.0, kus = 1.0 / kmhz;
        double base = 1e300; for (unsigned i : ok) base = std::min(base, R[i * 8] * kus - cqe_ts(RCQ_OFF, i) * us);
        std::vector<double> rxs, hop, bld, ack, bfi, work, lag, fixed;
        for (unsigned i : ok) {
          const uint64_t *a = &R[i * 8], *b = &T[i * 8];
          const double e = (cqe_ts(TCQ_OFF, i) - cqe_ts(RCQ_OFF, i)) * us, l = a[0] * kus - cqe_ts(RCQ_OFF, i) * us - base;
          rxs.push_back((a[1] - a[0]) * kus); hop.push_back((b[0] - a[0]) * kus); bld.push_back((b[1] - b[0]) * kus);
          ack.push_back((b[2] - b[1]) * kus); bfi.push_back((b[3] - b[2]) * kus); work.push_back((b[3] - a[0]) * kus);
          lag.push_back(l); fixed.push_back(e - (b[3] - a[0]) * kus - l);
        }
        printf("E5.1c stamps: %zu frames, kernel clock %.2f MHz (fitted to the ConnectX clock), pre %u, reap gap %u, %s RX slots\n", ok.size(), kmhz, spec, gap, DATA_OFF < 0x100000 ? "URAM" : "DDR");
        st3(rxs,  "rx: CQE seen -> frame streamed");
        st3(hop,  "rx CQE seen -> tx first beat (read+afu)");
        st3(bld,  "tx: first beat -> WQE+DBR issued");
        st3(ack,  "tx: acknowledgement wait");
        st3(bfi,  "tx: BlueFlame/doorbell issued");
        st3(work, "kernel work (CQE seen -> ring)");
        st3(lag,  "RX detection lag beyond fastest frame");
        st3(fixed,"echo - kernel work - RX lag excess");
      }
    }
  }
  const bool pass = got == n && good == n && !rxerr && r[1] == n && t[0] == n && !r[2] && !q[1] &&
                    s1 == ERT_CMD_STATE_COMPLETED && s2 == ERT_CMD_STATE_COMPLETED && s3 == ERT_CMD_STATE_COMPLETED;
  printf("RESULT %s (E5.1 FLD-lite: fld_rx -> afu_loop -> fld_tx, no CPU in the data path)\n", pass ? "PASS" : "FAIL");
  pw[1] = 0;
  ibv_destroy_flow(flow); ibv_destroy_qp(eqp); ibv_destroy_qp(sqp); ibv_destroy_cq(ecq); ibv_destroy_cq(scq);
  ibv_dereg_mr(rmr); ibv_dereg_mr(tmr); ibv_dealloc_pd(p1);
  e1_destroy_sq(&sq); e3_cq_destroy(&tcq); e2_rx_destroy(&rx); mlx5dv_devx_free_uar(uar); mlx5dv_devx_umem_dereg(umem);
  ibv_dereg_mr(dmr); ibv_dealloc_pd(pd); ibv_close_device(c0); ibv_close_device(c1);
  return pass ? 0 : 1;
}
