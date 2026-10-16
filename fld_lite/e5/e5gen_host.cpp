// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5 generator host (2026-09-30): FLD-lite + the loopback AFU on port 0 exactly as e5_host, but port 1 is driven BY THE
// FPGA: gen_tx posts frames on port 1's SQ (rings in a second FPGA DDR region, doorbells through PEER window 1) and
// sink_rx receives the echoes on port 1's RQ, counts them and byte-checks a sample against precomputed replies.
// No host CPU and no host PCIe link in the data path; both ports timestamp on the same ConnectX clock.
//   usage (root): e5gen_host <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF of port0> [frames] [options]
//     --len L | --mix   frame length 60..1500 (default 1024) or K templates of mixed lengths
//     --gap N           gen_tx pacing: idle cycles after each batch (0 = as fast as possible)
//     --gbatch B        gen_tx WQEs per doorbell (default 16);  --k K  templates (power of two, default 64)
//     --gbf             gen_tx posts single-WQE batches by BlueFlame (use with --gbatch 1);  --gnozero  gen_tx without zero-on-reap
//     --sample S        sink_rx checks every 2^S-th frame byte-for-byte (default 0 = all)
//     --swap            FLD-lite on port 1, the generator on port 0;  --gfrm-uram  generator frames in URAM (--jumbo, K <= 32)
//     --gring-uram      (with --jumbo) generator/sink rings in the URAM window instead of DDR
//     --jumbo           frames up to 9000 B / ZUC payloads up to 8192 B: 16 KB slots, port-0 RX slots in DDR (needs MTU 9000)
//     --zuc             E5.2 xclbin (ZUC AFU instead of afu_loop): the templates are ZUC requests (message ID = template),
//                       expected replies precomputed with zuc/zuc_model.h; --op conf|integ|mix, --zlen L payload bytes
//     FLD-lite options as e5_host: --bf --inline N --sig K --pre N --gap-reap N --maxb N
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
#include "zuc/zuc_model.h"

static size_t BUF = 8 << 20, BUF2 = 8 << 20, STRIDE = 2048, FSTR = 2048;   // FSTR: template / expected-reply stride
static int LOG_RQ = 8, LOG_TXS = 10; static const int LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11;
static size_t DATA_OFF = 0x80000;                         // RX slots in the URAM window (E5.1c default)
static const size_t UMEM_OFF = 0x0, UMEM_LEN = 0x60000, STOP_OFF = 0x380000;
static size_t TXB_OFF = 0x400000;
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const size_t SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const uint16_t ET_IN = 0x88B6, ET_OUT = 0x88B8;
static const size_t PW_OFF = 0x100000;
// region 2 (port 1 / generator), its own 8 MB P2P BO
static const size_t G_RQ = 0x0, G_RQ_DBR = 0x4000, G_RCQ_DBR = 0x4040, G_SQ_DBR = 0x4080, G_TCQ_DBR = 0x40C0;
static const size_t G_SQ = 0x10000, G_RCQ = 0x20000, G_TCQ = 0x60000, G_UMEM_LEN = 0x80000, G_TMPL = 0xA0000, G_ELEN = 0xA8000;
static const size_t G_STOP = 0xB0000, G_DATA = 0x100000;
static size_t G_FRM = 0x400000, G_EXP = 0x600000;
static int G_LOG_RQ = 10; static const int G_LOG_RCQ = 12, G_LOG_SQ = 10, G_LOG_TCQ = 11;
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
  bool lat = false, burst = false; unsigned bf = 0, inl = 0, sig = 16, stamp = 0, spec = 2, gap = 64, maxb = 16, rxk = 0, linger = 0;
  unsigned gflags = 0, ggap = 0, gbatch = 16, log_k = 6, log_sample = 0, zlen = 0; bool zuc = false, jumbo = false, guram = false, swapp = false, gfu = false; int zop = -1;
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
    if (!strcmp(argv[i], "--rxk") && i + 1 < argc) rxk = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--linger") && i + 1 < argc) linger = atoi(argv[i + 1]);      // E5.4: fld_tx batch linger cycles           // E5.3: CQEs per fld_rx poll (0/1 = one)
    if (!strcmp(argv[i], "--maxb") && i + 1 < argc) maxb = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gap-reap") && i + 1 < argc) gap = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gap") && i + 1 < argc) ggap = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gbatch") && i + 1 < argc) gbatch = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gbf")) gflags |= 1;
    if (!strcmp(argv[i], "--gnozero")) gflags |= 2;
    if (!strcmp(argv[i], "--k") && i + 1 < argc) { unsigned k = atoi(argv[i + 1]); log_k = 0; while ((2u << log_k) <= k && log_k < 8) log_k++; }
    if (!strcmp(argv[i], "--sample") && i + 1 < argc) log_sample = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--zuc")) zuc = true;
    if (!strcmp(argv[i], "--jumbo")) jumbo = true;
    if (!strcmp(argv[i], "--gring-uram")) guram = true;
    if (!strcmp(argv[i], "--swap")) swapp = true;
    if (!strcmp(argv[i], "--gfrm-uram")) gfu = true;
    if (!strcmp(argv[i], "--zlen") && i + 1 < argc) zlen = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--op") && i + 1 < argc) zop = !strcmp(argv[i + 1], "conf") ? 0 : !strcmp(argv[i + 1], "integ") ? 1 : -1;
  }
  if (jumbo) {
    // Option A (2026-09-30): frames up to 9000 B (ZUC requests up to 8 KB payload). 16 KB slots everywhere; port-0 RX slots
    // move from the URAM window to DDR (256 x 16 KB at 4 MB), TX buffer slots 256 x 16 KB at 8 MB, generator region 16 MB.
    // Both ConnectX ports need MTU 9000 (ip link set dev <if> mtu 9000), or the NIC drops the large frames.
    STRIDE = FSTR = 16384; BUF = BUF2 = 16 << 20; DATA_OFF = 0x400000; LOG_RQ = 8; TXB_OFF = 0x800000; LOG_TXS = 8;
    G_LOG_RQ = 8; G_FRM = 0x800000; G_EXP = 0xC00000;
    if (log_k > 8) { fprintf(stderr, "--jumbo allows at most 256 templates\n"); return 2; }
  }
  if (gfu && (!jumbo || guram || log_k > 5)) { fprintf(stderr, "--gfrm-uram needs --jumbo, not --gring-uram, and at most 32 templates (--k 32)\n"); return 2; }
  if (guram && !jumbo) { fprintf(stderr, "--gring-uram needs --jumbo (it uses the URAM space --jumbo frees)\n"); return 2; }
  if (FRAME < 60 || FRAME > (jumbo ? 9000u : 1500u)) { fprintf(stderr, "--len must be 60..%u\n", jumbo ? 9000u : 1500u); return 2; }
  if (zuc && zlen > (jumbo ? 8192u : 1372u)) { fprintf(stderr, "--zlen must be <= %u%s\n", jumbo ? 8192u : 1372u, jumbo ? "" : " (use --jumbo for more)"); return 2; }
  if ((inl & 0xffff) > 222) inl = (inl & ~0xffffu) | 222;       // clamp the size only: bit 16 = E5.5 adaptive inline
  if (lat) sig = 1;
  if (sig < 1) sig = 1;
  if (n > 100000000) n = 100000000;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krx(dev, uuid, "fld_rx"), ktx(dev, uuid, "fld_tx"), krp(dev, uuid, "fld_reap");
  xrt::kernel kgen(dev, uuid, "gen_tx"), ksnk(dev, uuid, "sink_rx");
  // the ring/frame BO first, so it lands at bank0 offset 0 (the HYBRID URAM window)
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, krx.group_id(0));
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, ktx.group_id(2));
  xrt::bo rres(dev, 4096, xrt::bo::flags::normal, krx.group_id(2)), tres(dev, 4096, xrt::bo::flags::normal, ktx.group_id(3)),
          qres(dev, 4096, xrt::bo::flags::normal, krp.group_id(1));
  xrt::bo rlog(dev, 4096 * 64, xrt::bo::flags::normal, krx.group_id(15)), tlog(dev, 4096 * 64, xrt::bo::flags::normal, ktx.group_id(22));
  uint8_t *base = bo.map<uint8_t *>(), *um = base + UMEM_OFF;
  if (bo.address() != 0x200000000ull) { printf("*** ring BO at 0x%llx, not bank0 offset 0 (URAM window)\n", (unsigned long long)bo.address()); return 1; }
  xrt::bo bo2(dev, BUF2, xrt::bo::flags::p2p, kgen.group_id(0));
  xrt::bo gres(dev, 4096, xrt::bo::flags::normal, kgen.group_id(2)), sres(dev, 4096, xrt::bo::flags::normal, ksnk.group_id(2));
  uint8_t *g = bo2.map<uint8_t *>();
  // Generator/sink rings: in region 2 (DDR) by default. --gring-uram puts them in the URAM window of region 1 at 0x80000
  // (free in --jumbo, whose port-0 RX slots are in DDR), and the WQE templates / lengths / stop word in region 1's free
  // DDR at 0xC00000. gen_tx and sink_rx then address region 1, and reach region 2's frames and slots at offset D2.
  uint8_t *grg = g, *gt = g; xrt::bo *kbo = &bo2; size_t GR_OFF = 0, T_SHIFT = 0, D2 = 0;
  if (guram) {
    if (bo2.address() <= bo.address() || bo2.address() - bo.address() > 0xF0000000ull) { printf("*** region 2 not addressable from region 1\n"); return 1; }
    grg = base + 0x80000; GR_OFF = 0x80000; T_SHIFT = 0xC00000 - G_TMPL; gt = base + T_SHIFT; kbo = &bo;
    D2 = bo2.address() - bo.address();
  }

  // --swap: FLD-lite on the second port (function .1), the generator on the first
  std::string bdf_fld = argv[5], bdf_gen = argv[5]; bdf_fld.back() = swapp ? '1' : '0'; bdf_gen.back() = swapp ? '0' : '1';
  ibv_context *c0 = open_dev(swapp ? argv[4] : argv[3], true), *c1 = open_dev(swapp ? argv[3] : argv[4], true);   // c0 = FLD-lite, c1 = generator
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

  const uint64_t uar_bus = bar_start(bdf_fld, 0) + (uint64_t)uar->page_id * 4096 + ((uint8_t *)uar->reg_addr - (uint8_t *)uar->base_addr);
  const unsigned db_off = (unsigned)((uar_bus & 0xFFFFF) - (pbo.address() & 0xFFFFF));
  int fd = open(("/sys/bus/pci/devices/" + std::string(argv[1]) + "/resource0").c_str(), O_RDWR | O_SYNC);
  volatile uint32_t *pw = (volatile uint32_t *)((uint8_t *)mmap(nullptr, 1 << 25, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) + PW_OFF);
  if (pw[0] != 0x50454552) { printf("*** peer_window not found -- is the E3 shell running?\n"); return 1; }
  pw[1] = 0; pw[2] = (uint32_t)(uar_bus & ~0xFFFFFull); pw[3] = (uint32_t)(uar_bus >> 32);
  const uint32_t w0 = pw[5]; pw[1] = 1;
  if (pw[8] < 4) { printf("*** the generator needs peer_window v1.1 (4 windows; NWIN reads %u)\n", pw[8]); return 1; }
  printf("E5.1 FLD-lite: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%llx via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, %s\n",
         rx.rqn, rx.cqn, sq.sqn, tcq.cqn, (unsigned long long)uar_bus, db_off, bf ? "on" : "off", inl, sig,
         MIX ? "lengths 60..1500 mixed" : (std::to_string(FRAME) + " B frames").c_str());

  // engines: stream args (fld_rx 3; fld_tx 4,5; fld_reap 2) are not set by the host
  xrt::run rrx(krx), rtx(ktx), rrp(krp);
  rrx.set_arg(0, bo); rrx.set_arg(1, bo); rrx.set_arg(2, rres);
  { int a = 4; for (unsigned v : {(unsigned)(UMEM_OFF + RCQ_OFF), (unsigned)LOG_RCQ, (unsigned)(UMEM_OFF + RCQ_DBR), (unsigned)(UMEM_OFF + RQ_DBR),
                                  (unsigned)LOG_RQ, (unsigned)DATA_OFF, (unsigned)STRIDE, (unsigned)STOP_OFF, 16u, (spec & 0xff) | ((rxk & 0xff) << 8), stamp}) rrx.set_arg(a++, v);
    rrx.set_arg(15, rlog); }
  rtx.set_arg(0, bo); rtx.set_arg(1, bo); rtx.set_arg(2, pbo); rtx.set_arg(3, tres);
  { int a = 6;
    for (unsigned v : {(unsigned)(UMEM_OFF + SQ_OFF), (unsigned)LOG_SQ, (unsigned)(UMEM_OFF + SQ_DBR), sq.sqn, dmr->lkey, (unsigned)TXB_OFF}) rtx.set_arg(a++, v);
    rtx.set_arg(a++, (unsigned long long)(uintptr_t)(base + TXB_OFF));
    for (unsigned v : {(unsigned)LOG_TXS, (unsigned)STRIDE, db_off, bf, 256u, inl, sig, (unsigned)STOP_OFF, stamp}) rtx.set_arg(a++, v);
    rtx.set_arg(22, tlog); rtx.set_arg(25, (maxb & 0xff) | (linger << 8)); }
  rrp.set_arg(0, bo); rrp.set_arg(1, qres);
  { int a = 3; for (unsigned v : {(unsigned)(UMEM_OFF + TCQ_OFF), (unsigned)LOG_TCQ, (unsigned)(UMEM_OFF + TCQ_DBR), (unsigned)STOP_OFF, gap,
                                  (unsigned)(UMEM_OFF + SQ_OFF), getenv("FLD_NO_ZERO") ? 0u : (unsigned)LOG_SQ}) rrp.set_arg(a++, v); }   // zero-on-reap
  rrp.start(); rtx.start(); rrx.start();

  // ---------------------------------------------------------------- port 1: the FPGA traffic generator
  std::string bdf1 = bdf_gen;                                       // the generator's ConnectX function
  const unsigned K = 1u << log_k;
  ibv_pd *pd1 = ibv_alloc_pd(c1);
  ibv_mr *gmr = ibv_reg_mr(pd1, g + G_DATA, std::min(G_EXP + FSTR * 256, BUF2) - G_DATA, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *gum = mlx5dv_devx_umem_reg(c1, grg, G_UMEM_LEN, IBV_ACCESS_LOCAL_WRITE);
  if (!gmr || !gum) { printf("port 1 registration failed: %s\n", strerror(errno)); return 1; }
  // --gfrm-uram: the generator's request frames in the URAM window at 0x80000 (free in --jumbo without --gring-uram)
  ibv_mr *fmr = gfu ? ibv_reg_mr(pd1, base + 0x80000, 0x80000, IBV_ACCESS_LOCAL_WRITE) : nullptr;
  if (gfu && !fmr) { printf("URAM frame registration failed: %s\n", strerror(errno)); return 1; }
  uint8_t *gf = gfu ? base + 0x80000 - G_FRM : g;                   // gf + G_FRM + k*FSTR = the frame
  const uint32_t flkey = gfu ? fmr->lkey : gmr->lkey;
  mlx5dv_pd dpd1{}; mlx5dv_obj o1{}; o1.pd.in = pd1; o1.pd.out = &dpd1; mlx5dv_init_obj(&o1, MLX5DV_OBJ_PD);
  mlx5dv_devx_uar *uar1 = mlx5dv_devx_alloc_uar(c1, MLX5DV_UAR_ALLOC_TYPE_NC);
  memset(grg, 0, G_UMEM_LEN); memset(gt + G_STOP, 0, 64);
  for (unsigned i = 0; i < (1u << G_LOG_RCQ); i++) grg[G_RCQ + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << G_LOG_TCQ); i++) grg[G_TCQ + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << G_LOG_RQ); i++)
    mlx5dv_set_data_seg((mlx5_wqe_data_seg *)(grg + G_RQ + i * 16), STRIDE, gmr->lkey, (uintptr_t)(g + G_DATA + i * STRIDE));
  // K frames (echo requests, ethertype ET_IN, template index at byte 14), WQE templates, expected echoes, lengths
  uint16_t *elen = (uint16_t *)(gt + G_ELEN);
  static const unsigned ZLENS[] = {1, 13, 64, 65, 100, 128, 200, 511, 1000, 1372};
  auto rng = [](unsigned i, unsigned k) { unsigned x = i * 2654435761u ^ (k * 40503u + 0x9e3779b9u); x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; return x; };
  uint64_t zpay = 0;
  for (unsigned k = 0; k < K; k++) {
    uint8_t *f = gf + G_FRM + k * FSTR;
    if (zuc) {
      // ZUC request k: Eth/IPv4/UDP beat (IP checksum non-zero), ZUC header beat, payload (see e52_host.cpp)
      static const unsigned ZLENS_J[] = {1, 64, 511, 1372, 2048, 3000, 4096, 6000, 8000, 8192};
      const unsigned op = zop >= 0 ? (unsigned)zop : (k & 1);
      unsigned L = zlen ? zlen : (jumbo ? ZLENS_J : ZLENS)[(k / 2) % 10];
      if (op == 1 && L > 8191) L = 8191;                          // INTEG's bit counter is 16 bits: 8191 bytes at most
      const unsigned fl = 128 + L;
      memset(f, 0, FSTR);
      memcpy(f, "\x02\x49\x4e\x4e\x32\x00", 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF;
      uint8_t *ip = f + 14;
      ip[0] = 0x45; ip[2] = (fl - 14) >> 8; ip[3] = fl - 14; ip[5] = k; ip[6] = 0x40; ip[8] = 64; ip[9] = 17;
      ip[12] = 10; ip[15] = 2; ip[16] = 10; ip[19] = 1;
      { uint32_t cs = 0; for (int q = 0; q < 20; q += 2) cs += (ip[q] << 8) | ip[q + 1]; cs = (cs & 0xffff) + (cs >> 16); cs = (cs & 0xffff) + (cs >> 16); cs = ~cs & 0xffff; ip[10] = cs >> 8; ip[11] = cs; }
      uint8_t *u = f + 34; u[0] = 0x30; u[1] = 0x39; u[2] = 0x16; u[3] = 0x2e; u[4] = (fl - 34) >> 8; u[5] = fl - 34;
      for (int q = 42; q < 64; q++) f[q] = 0xAA;
      uint8_t *h = f + 64; h[0] = op; h[1] = 0x5A; h[2] = L >> 8; h[3] = L;
      for (int q = 0; q < 8; q++) h[4 + q] = (uint64_t)k >> (8 * (7 - q));
      for (int q = 0; q < 16; q++) { h[12 + q] = rng(k, q); h[28 + q] = rng(k, 100 + q); }
      for (int q = 44; q < 64; q++) h[q] = 0xCC;
      for (unsigned q = 0; q < L; q++) f[128 + q] = rng(k, 1000 + q);
      // expected reply (zuc_afu_port_spec.md 5): L2-L4 swapped, IP/UDP lengths, both checksums 0, header echo, result
      uint8_t *e = g + G_EXP + k * FSTR; memset(e, 0, FSTR);
      memcpy(e, f + 6, 6); memcpy(e + 6, f, 6); e[12] = f[12]; e[13] = f[13];
      memcpy(e + 14, f + 14, 20); memcpy(e + 26, f + 30, 4); memcpy(e + 30, f + 26, 4); e[24] = e[25] = 0;
      const unsigned ipl = op == 0 ? L + 0x72 : 0x62, udl = op == 0 ? L + 0x5E : 0x4E;
      e[16] = ipl >> 8; e[17] = ipl; memcpy(e + 34, f + 36, 2); memcpy(e + 36, f + 34, 2); e[38] = udl >> 8; e[39] = udl;
      memcpy(e + 64, f + 64, 12);
      if (op == 0) { zuc_xor(h + 12, h + 28, f + 128, e + 128, L); elen[k] = 128 + L; }
      else { const uint32_t m = eia3_mac(h + 12, h + 28, f + 128, 8 * L); e[108] = m >> 24; e[109] = m >> 16; e[110] = m >> 8; e[111] = m; elen[k] = 112; }
      zpay += L;
      uint8_t *w = gt + G_TMPL + k * 64; memset(w, 0, 64);
      w[7] = 4; w[29] = 18; memcpy(w + 30, f, 18);
      *(uint32_t *)(w + 48) = htobe32(fl - 18); *(uint32_t *)(w + 52) = htobe32(flkey);
      *(uint64_t *)(w + 56) = htobe64((uint64_t)(uintptr_t)(f + 18));
      continue;
    }
    const unsigned L = MIX ? flen(k) : FRAME;
    for (unsigned q = 0; q < L; q++) f[q] = (uint8_t)(q * 13 + k * 5 + (q >> 8));
    memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF; f[14] = k;
    uint8_t *e = g + G_EXP + k * FSTR; memcpy(e, f, L); e[6] = 0x02; e[11] = 0xEC; e[12] = ET_OUT >> 8; e[13] = ET_OUT & 0xFF;
    elen[k] = L;
    uint8_t *w = gt + G_TMPL + k * 64; memset(w, 0, 64);
    w[7] = 4; w[29] = 18; memcpy(w + 30, f, 18);                    // ctrl ds=4; eth seg: 18-byte inline header
    *(uint32_t *)(w + 48) = htobe32(L - 18); *(uint32_t *)(w + 52) = htobe32(flkey);
    *(uint64_t *)(w + 56) = htobe64((uint64_t)(uintptr_t)(f + 18));
  }
  _mm_sfence();
  e2_rx_cfg gcfg{dpd1.pdn, uar1->page_id, gum->umem_id, G_RCQ, G_RCQ_DBR, G_RQ, G_RQ_DBR, G_LOG_RCQ, G_LOG_RQ, zuc ? ET_IN : ET_OUT};   // ZUC replies keep the request ethertype
  e2_rx grx;
  if (e2_rx_create(c1, &gcfg, &grx, err, sizeof(err))) { printf("*** port 1 RX: %s\n", err); return 1; }
  *(volatile uint32_t *)(grg + G_RQ_DBR) = htobe32(1u << G_LOG_RQ);
  e3_cq gtcq; e1_sq gsq;
  if (e3_cq_create(c1, uar1->page_id, gum->umem_id, G_TCQ, G_TCQ_DBR, G_LOG_TCQ, &gtcq, err, sizeof(err))) { printf("*** port 1 TX CQ: %s\n", err); return 1; }
  int mi1 = e1_query_min_inline(c1, err, sizeof(err)); if (mi1 < 0) mi1 = 1;
  if (e1_create_sq(c1, dpd1.pdn, gtcq.cqn, uar1->page_id, gum->umem_id, G_SQ, gum->umem_id, G_SQ_DBR, G_LOG_SQ, mi1, &gsq, err, sizeof(err))) { printf("*** port 1 SQ: %s\n", err); return 1; }
  _mm_sfence();
  const uint64_t uar1_bus = bar_start(bdf1, 0) + (uint64_t)uar1->page_id * 4096 + ((uint8_t *)uar1->reg_addr - (uint8_t *)uar1->base_addr);
  const unsigned db_off1 = (unsigned)((uar1_bus & 0xFFFFF) + (1u << 20) - (pbo.address() & 0xFFFFF));
  // PEER window 1 -> port 1 UAR. The peer_window ignores base writes while enabled (CTRL=1), so disable, set, re-enable
  pw[1] = 0;
  pw[0x28 / 4] = (uint32_t)(uar1_bus & ~0xFFFFFull); pw[0x2C / 4] = (uint32_t)(uar1_bus >> 32);
  pw[1] = 1;
  if (pw[0x28 / 4] != (uint32_t)(uar1_bus & ~0xFFFFFull)) { printf("*** peer window 1 base did not stick (0x%08x)\n", pw[0x28 / 4]); return 1; }
  printf("generator: port 1 (%s) sqn 0x%x rqn 0x%x; UAR 0x%llx via PEER window 1 +0x%x; K=%u templates, batch %u, gap %u, check 1/%u, BlueFlame %s, zero-on-reap %s\n",
         bdf1.c_str(), gsq.sqn, grx.rqn, (unsigned long long)uar1_bus, db_off1, K, gbatch, ggap, 1u << log_sample,
         (gflags & 1) ? (gbatch == 1 ? "on" : "on (single-WQE batches only)") : "off", (gflags & 2) ? "off" : "on");

  xrt::run rgen(kgen), rsnk(ksnk);
  rgen.set_arg(0, *kbo); rgen.set_arg(1, pbo); rgen.set_arg(2, gres);
  { int a = 3; for (unsigned v : {(unsigned)(G_TMPL + T_SHIFT), log_k, (unsigned)(G_SQ + GR_OFF), (unsigned)G_LOG_SQ, (unsigned)(G_SQ_DBR + GR_OFF), gsq.sqn,
                                  (unsigned)(G_TCQ + GR_OFF), (unsigned)G_LOG_TCQ, (unsigned)(G_TCQ_DBR + GR_OFF), db_off1, n, gbatch, ggap, (unsigned)(getenv("GEN_SIG") ? atoi(getenv("GEN_SIG")) : 64),
                                  (unsigned)(G_STOP + T_SHIFT), gflags, 256u}) rgen.set_arg(a++, v); }
  rsnk.set_arg(0, *kbo); rsnk.set_arg(1, *kbo); rsnk.set_arg(2, sres);
  { int a = 3; for (unsigned v : {(unsigned)(G_RCQ + GR_OFF), (unsigned)G_LOG_RCQ, (unsigned)(G_RCQ_DBR + GR_OFF), (unsigned)(G_RQ_DBR + GR_OFF), (unsigned)G_LOG_RQ,
                                  (unsigned)(G_DATA + D2), (unsigned)STRIDE, (unsigned)(G_EXP + D2), (unsigned)FSTR, (unsigned)(G_ELEN + T_SHIFT),
                                  log_k, log_sample, n, (unsigned)(G_STOP + T_SHIFT), zuc ? 19u : 14u}) rsnk.set_arg(a++, v); }   // template id: ZUC = IP identification low byte (the reply echoes the IP header; kernels built before the fix read only beat 0)
  const uint32_t pwf0 = pw[5];
  rsnk.start();
  auto t0 = std::chrono::steady_clock::now();
  rgen.start();
  auto sg = rgen.wait(std::chrono::milliseconds(60000));
  const double gsecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  usleep(300000);                                                   // let the last frames through
  *(volatile uint32_t *)(gt + G_STOP) = 1; *(volatile uint32_t *)(base + STOP_OFF) = 1; _mm_sfence();
  auto ss = rsnk.wait(std::chrono::milliseconds(10000));
  auto s1 = rrx.wait(std::chrono::milliseconds(10000)), s2 = rtx.wait(std::chrono::milliseconds(10000)), s3 = rrp.wait(std::chrono::milliseconds(10000));
  rres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); tres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); qres.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  gres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); sres.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  uint32_t r[8], t[16], q[8], gr[8]; uint64_t sr[10];
  rres.read(r, sizeof(r), 0); tres.read(t, sizeof(t), 0); qres.read(q, sizeof(q), 0); gres.read(gr, sizeof(gr), 0); sres.read(sr, sizeof(sr), 0);
  auto dn = [](ert_cmd_state s) { return s == ERT_CMD_STATE_COMPLETED ? "done" : "NOT done"; };
  printf("gen_tx  (%s): posted %u in %u doorbells, TX CQEs %u, TX errors %u, credit polls %u  (%.3f s wall)\n", dn(sg), gr[0], gr[1], gr[2], gr[3], gr[5], gsecs);
  if (gr[3]) printf("  first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x\n", gr[4] >> 28, (gr[4] >> 16) & 0xFF, (gr[4] >> 8) & 0xFF);
  if (gr[3] && getenv("E5_DUMP_ERR")) {                               // diagnostics: the error CQE and the WQEs around it
    for (unsigned i = 0; i < (1u << G_LOG_TCQ); i++) {
      const uint8_t *c = grg + G_TCQ + i * 64;
      if ((c[63] >> 4) != 0xD || c[55] == 0x05) continue;              // the first REAL error (0x05 = flushed after it)
      const unsigned wc = (c[60] << 8) | c[61];
      printf("  TX CQ[%u]: opcode 13 synd 0x%02x vendor 0x%02x wqe_counter %u (sq slot %u); pi at end %u\n", i, c[55], c[54], wc,
             wc & ((1u << G_LOG_SQ) - 1), gr[0]);
      for (int d = -2; d <= 1; d++) {
        const unsigned slot = (wc + d) & ((1u << G_LOG_SQ) - 1); const uint8_t *w = grg + G_SQ + slot * 64;
        printf("    WQE slot %4u:", slot); for (int b = 0; b < 64; b++) printf("%s%02x", b % 16 ? "" : " ", w[b]); printf("\n");
      }
      for (unsigned j = i >= 2 ? i - 2 : 0; j < i; j++) { const uint8_t *p2 = grg + G_TCQ + j * 64; printf("    prev CQ[%u]: op %u wqe_counter %u\n", j, p2[63] >> 4, (p2[60] << 8) | p2[61]); }
      break;
    }
  }
  printf("fld_rx  (%s): CQEs %u, frames %u, RX errors %u | fld_tx (%s): frames %u, batches %u | fld_reap (%s): CQEs %u, errors %u, SQ slots zeroed %u\n",
         dn(s1), r[0], r[1], r[2], dn(s2), t[0], t[6], dn(s3), q[0], q[1], q[5]);
  printf("fld_rx  polls %u, polls with >1 CQE %u, longest run %u, polls issued with K>1 %u (rxk %u)\n", r[4], r[5], r[6], r[7], rxk);
  printf("fld_tx  batches %u, frames per batch %.2f, joined by linger %u, largest batch %u, closed early for room %u, inline frames %u (linger %u)\n", t[6], t[6] ? (double)t[0] / t[6] : 0.0, t[7], t[8], t[9], t[2], linger);
  printf("sink_rx (%s): frames %llu, bytes %llu, RX error CQEs %llu, checked %llu, MISMATCHES %llu, length mismatches %llu\n", dn(ss),
         (unsigned long long)sr[0], (unsigned long long)sr[1], (unsigned long long)sr[2], (unsigned long long)sr[3],
         (unsigned long long)sr[4], (unsigned long long)sr[9]);
  printf("peer window: writes forwarded %u, refused %u (counter totals)\n", pw[5] - pwf0, pw[7]);
  ibv_device_attr_ex da{}; ibv_query_device_ex(c1, nullptr, &da);
  const double us = 1000.0 / (double)da.hca_core_clock;
  const unsigned lost_fld = gr[0] - r[0], lost_after = r[1] - (unsigned)sr[0];
  printf("loss: %u before FLD-lite (port 0 RQ empty), %u after it\n", lost_fld, lost_after);
  if (sr[0] > 1 && sr[6] > sr[5]) {
    const double d = (sr[6] - sr[5]) * us * 1e-6, fps = (sr[0] - 1) / d;
    const double avg = (double)sr[1] / sr[0], wire = fps * (avg + 24) * 8e-9;   // + preamble/SFD/FCS/IFG (8+4+12)
    printf("RATE through FLD-lite: %.3f M frames/s, %.2f Gb/s of frames (%.2f Gb/s on the wire incl. FCS/preamble/IFG; 10 GbE line)\n",
           fps * 1e-6, fps * avg * 8e-9, wire);
    if (zuc) printf("ZUC: %.3f M requests/s, %.2f Gb/s of ZUC payload\n", fps * 1e-6, fps * ((double)zpay / K) * 8e-9);
  }
  const bool pass = gr[0] == n && !gr[3] && sr[4] == 0 && sr[9] == 0 && sr[2] == 0 && lost_after == 0 && sr[3] > 0 &&
                    sg == ERT_CMD_STATE_COMPLETED && ss == ERT_CMD_STATE_COMPLETED && s1 == ERT_CMD_STATE_COMPLETED &&
                    s2 == ERT_CMD_STATE_COMPLETED && s3 == ERT_CMD_STATE_COMPLETED;
  printf("RESULT %s (FPGA generator -> FLD-lite -> FPGA sink: nothing corrupted, nothing lost after FLD-lite%s)\n", pass ? "PASS" : "FAIL",
         lost_fld ? "; loss before it = offered load above FLD-lite's rate" : ", no loss at all");
  pw[1] = 0;
  return pass ? 0 : 1;
}
