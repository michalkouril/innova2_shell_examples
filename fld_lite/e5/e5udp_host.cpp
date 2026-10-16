// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// e5udp_host -- UDP architecture step 1 (2026-10-03): FLD-lite + udp_swap as a UDP echo endpoint ("UDP ping"). Same
// ConnectX queues as e5_host (DevX RQ/RX CQ/TIR + SQ/TX CQ, rings in the bank0 URAM window, frames in DDR, doorbells via
// PEER), but the NIC_RX rule is {IPv4, UDP, dst ip, dst port, !frag} + flow TAG (step 0), fld_rx drops frames whose CQE
// says bad checksum / IP options (pre bit 17) and fld_tx sets cs_flags L3|L4 (maxb bits 31:24). No traffic generator:
// the engines run for --secs while an external client (Linux sockets on another port) sends datagrams; then counters.
//   usage (root): e5udp_host <user BDF> <xclbin> <fld dev> <cx5 BDF of that port> [--secs S] [--ip A.B.C.D] [--port P]
//                 [--rxk K] [--pre N] [--inline N] [--inline-adapt] [--bf] [--no-rxchk] [--no-csum]
//                 [--jumbo]  frames up to 9000 B (as e5gen_host --jumbo): 16 KB RX/TX slots, RX slots in DDR at 4 MB,
//                            TX slots at 8 MB, 16 MB BO; both ConnectX ports need MTU 9000
//                 [--small]  E5.6 layout A: every ring inside the first 512 KB (PLRAM0/1/2 images), 64 RX slots,
//                            1024-entry RX/TX CQs, per-engine stop words; all in the bank0 URAM window (no alias)
//                 [--plram]  E5.6 layout B (fp7 only; implies --small): the same offsets, but the fp7 p2p_alias
//                            window (BAR offset 0) maps them to PLRAM0/1/2 -- the ConnectX reaches them through the
//                            P2P BAR, the kernels through their own PLRAM ports (link: fld_rx.mem PLRAM2, fld_rx.dbr
//                            + fld_tx.sq PLRAM0, fld_reap.cq PLRAM1). TX buffers stay in bank0.
//   window offset map (--small): PLRAM0 [0,128K) RQ 0x0, DBRs 0x4000.., TX stop 0x5000, SQ 0x10000 | PLRAM1
//   [128K,256K) TCQ 0x20000, reap stop 0x30000, TCQ DBR 0x30040 | PLRAM2 [256K,512K) RCQ 0x40000, RX stop 0x50000,
//   RX slots 0x58000 (64 x 2 KB)
//                 [--stop-file F] end the run (cleanly, as at --secs) as soon as file F exists
//                 [--dump N] with --mcd: print the memcached debug counters every N s (epoch-stamped)
//                 [--mcd]    memcached xclbin (udp_rx_lite -> mcd -> udp_tx_lite): 64 KB hash table + 64 MB value store
//                            in bank0, handed to the host-started mcd_mem kernel before the steering rule admits
//                            traffic; use --port 11211
//                 [--slots128] with --small/--plram: 128 RX slots of 1536 B (a full 1514 B frame), 256-entry RX and TX CQs;
//                            PLRAM2 = RCQ 0x40000 (16 KB), RX stop 0x44000, RX slots 0x48000..0x78000. TX slots use the
//                            same 1536 B stride.
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
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include "devx_rx_s0.h"
#include "devx_sq.h"
#include "devx_cq.h"

static size_t BUF = 8 << 20, STRIDE = 2048; static const size_t RXLEN = 2048;
static int LOG_RQ = 8, LOG_TXS = 10, LOG_RCQ = 11, LOG_TCQ = 11; static const int LOG_SQ = 10;
static size_t DATA_OFF = 0x80000;                         // RX slots in the URAM window (E5.1c default)
static const size_t UMEM_OFF = 0x0, STOP_OFF = 0x380000; static size_t TXB_OFF = 0x400000, UMEM_LEN = 0x60000;
static size_t STOP_RX = STOP_OFF, STOP_TX = STOP_OFF, STOP_RP = STOP_OFF;   // per-engine stop words (--small moves them)
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080; static size_t TCQ_DBR = 0x40C0;
static const size_t SQ_OFF = 0x10000; static size_t RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const size_t ALIAS_OFF = 0x102000, PL_BASE = 0x2000000;   // fp7 p2p_alias (user BAR0) / PLRAM0 kernel address
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
static bool stop_req = false;
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
  if (argc < 5) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <fld dev> <cx5 BDF> [--secs S] [--ip A.B.C.D] [--port P] ...\n", argv[0]); return 2; }
  unsigned mcd_dump_s = 0; const char *stop_file = nullptr;
  unsigned n = 0, secs_run = 60, rxk = 4, port = 7777, rxchk = 1, csum = 1, latrep = 0;
  unsigned iperf = 0, iperf_ip = 0, iperf_words = 23, iperf_gap = 0, iperf_delay = 5;
  uint32_t ip = 0xC0A84D02;   // 192.168.77.2; iperf_ip is in wire byte order (s_addr)
  bool plram = false, slots128 = false, mcdk = false;
  bool lat = false, burst = false; unsigned bf = 0, inl = 0, sig = 16, stamp = 0, spec = 2, gap = 64, maxb = 16;
  for (int i = 5; i < argc; i++) {
    if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs_run = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--rxk") && i + 1 < argc) rxk = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--ip") && i + 1 < argc) { in_addr a; inet_pton(AF_INET, argv[i + 1], &a); ip = ntohl(a.s_addr); }
    if (!strcmp(argv[i], "--no-rxchk")) rxchk = 0;
    if (!strcmp(argv[i], "--no-csum")) csum = 0;
    if (!strcmp(argv[i], "--lat")) latrep = 1;
    if (!strcmp(argv[i], "--iperf") && i + 1 < argc) {             // fns iperf_udp client: IP,WORDS,GAP[,DELAY_S]
      char ipb[32] = {0}; unsigned w = 23, g = 0, dly = 5;
      if (sscanf(argv[i + 1], "%31[^,],%u,%u,%u", ipb, &w, &g, &dly) >= 1) {
        in_addr a; inet_pton(AF_INET, ipb, &a); iperf_ip = a.s_addr; iperf_words = w; iperf_gap = g; iperf_delay = dly; iperf = 1; }
    }
    if (!strcmp(argv[i], "--lat")) lat = true; if (!strcmp(argv[i], "--burst")) burst = true;
    if (!strcmp(argv[i], "--bf")) bf = 1; if (!strcmp(argv[i], "--mix")) MIX = true;
    if (!strcmp(argv[i], "--inline") && i + 1 < argc) inl = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--inline-adapt")) inl |= 1u << 16;          // E5.5: inline only lone frames on an idle link
    if (!strcmp(argv[i], "--len") && i + 1 < argc) FRAME = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--sig") && i + 1 < argc) sig = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--stamp")) stamp = 1;
    if (!strcmp(argv[i], "--rxddr")) { DATA_OFF = 0x100000; LOG_RQ = 10; }
    if (!strcmp(argv[i], "--small") || !strcmp(argv[i], "--plram")) {
      LOG_RQ = 6; LOG_RCQ = 10; LOG_TCQ = 10; TCQ_OFF = 0x20000; TCQ_DBR = 0x30040; STOP_RP = 0x30000; STOP_TX = 0x5000;
      RCQ_OFF = 0x40000; STOP_RX = 0x50000; DATA_OFF = 0x58000; UMEM_LEN = 0x50000; }
    if (!strcmp(argv[i], "--plram")) plram = true;
    if (!strcmp(argv[i], "--slots128")) slots128 = true;
    if (!strcmp(argv[i], "--mcd")) mcdk = true;
    if (!strcmp(argv[i], "--dump") && i + 1 < argc) mcd_dump_s = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--stop-file") && i + 1 < argc) stop_file = argv[i + 1];          // end the run early, cleanly   // --mcd: debug counters every N s
    if (!strcmp(argv[i], "--jumbo")) { STRIDE = 16384; BUF = 16 << 20; DATA_OFF = 0x400000; LOG_RQ = 8; TXB_OFF = 0x800000; LOG_TXS = 8; }
    if (!strcmp(argv[i], "--pre") && i + 1 < argc) spec = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--maxb") && i + 1 < argc) maxb = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atoi(argv[i + 1]);
  }
  if (slots128) {
    if (DATA_OFF != 0x58000) { fprintf(stderr, "--slots128 needs --small or --plram\n"); return 2; }
    LOG_RQ = 7; LOG_RCQ = 8; LOG_TCQ = 8; STRIDE = 1536; STOP_RX = 0x44000; DATA_OFF = 0x48000; UMEM_LEN = 0x44000;
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
  // (--plram: fld_rx.mem is a PLRAM port now, so the bank0 BO goes through fld_tx.buf's group)
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, plram ? ktx.group_id(1) : krx.group_id(0));
  xrt::bo pl0, pl1, pl2;
  if (plram) {
    pl0 = xrt::bo(dev, 0x20000, xrt::bo::flags::normal, krx.group_id(1));   // fld_rx.dbr / fld_tx.sq  -> PLRAM0
    pl1 = xrt::bo(dev, 0x20000, xrt::bo::flags::normal, krp.group_id(0));   // fld_reap.cq             -> PLRAM1
    pl2 = xrt::bo(dev, 0x40000, xrt::bo::flags::normal, krx.group_id(0));   // fld_rx.mem              -> PLRAM2
    if (pl0.address() != PL_BASE || pl1.address() != PL_BASE + 0x20000 || pl2.address() != PL_BASE + 0x40000) {
      printf("*** PLRAM BOs at 0x%llx/0x%llx/0x%llx, expected 0x2000000/0x2020000/0x2040000 (is this the --plram xclbin?)\n",
             (unsigned long long)pl0.address(), (unsigned long long)pl1.address(), (unsigned long long)pl2.address()); return 1; }
  }
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, ktx.group_id(2));
  xrt::bo rres(dev, 4096, xrt::bo::flags::normal, krx.group_id(2)), tres(dev, 4096, xrt::bo::flags::normal, ktx.group_id(3)),
          qres(dev, 4096, xrt::bo::flags::normal, krp.group_id(1));
  xrt::bo rlog(dev, 4096 * 64, xrt::bo::flags::normal, krx.group_id(15)), tlog(dev, 4096 * 64, xrt::bo::flags::normal, ktx.group_id(22));
  uint8_t *base = bo.map<uint8_t *>(), *um = base + UMEM_OFF;
  if (bo.address() != 0x200000000ull) { printf("*** ring BO at 0x%llx, not bank0 offset 0 (URAM window)\n", (unsigned long long)bo.address()); return 1; }
  // --mcd: the memcached memories, after the ring BO (which must stay at bank0 offset 0). mcd_mem is a host-started
  // kernel taking them as normal arguments (8 stream args first, then ht = 8, vs = 9); started now, never waited
  // for -- the free-running mcd core's start-up hash-table zeroing has been waiting in the streams for it.
  xrt::bo mht, mvs, mdbg; xrt::run mrun, drun;
  if (mcdk) {
    xrt::kernel kmem(dev, uuid, "mcd_mem");
    mht = xrt::bo(dev, 64u << 10, xrt::bo::flags::normal, kmem.group_id(8));
    mvs = xrt::bo(dev, 64u << 20, xrt::bo::flags::normal, kmem.group_id(9));
    // debug snapshots (mcd_dbg, args: 2 streams then out = 2) -- started before mcd_mem so nothing is missed
    try {
      xrt::kernel kdbg(dev, uuid, "mcd_dbg");
      mdbg = xrt::bo(dev, 4096, xrt::bo::flags::normal, kdbg.group_id(2));
      std::vector<uint8_t> z(4096, 0); mdbg.write(z.data()); mdbg.sync(XCL_BO_SYNC_BO_TO_DEVICE);
      drun = xrt::run(kdbg); drun.set_arg(2, mdbg); drun.start();
    } catch (const std::exception &e) { printf("memcached: no mcd_dbg kernel (%s)\n", e.what()); }
    mrun = xrt::run(kmem); mrun.set_arg(8, mht); mrun.set_arg(9, mvs); mrun.start();
    printf("memcached: mcd_mem started; hash table 64 KB @0x%llx, value store 64 MB @0x%llx\n",
           (unsigned long long)mht.address(), (unsigned long long)mvs.address());
  }
  auto mcd_dump = [&](const char *when) {
    if (!mdbg) return;
    uint64_t v[64]; mdbg.sync(XCL_BO_SYNC_BO_FROM_DEVICE); mdbg.read(v, sizeof(v), 0);
    static const char *nm[25] = {"", "rx beats", "rx words", "tx words", "tx responses", "ht rd cmds", "ht wr cmds", "vs rd cmds", "vs wr cmds",
                                 "", "", "", "", "", "", "", "mem ht rd cmds", "mem ht rd words", "mem ht wr cmds", "mem ht wr words",
                                 "mem vs rd cmds", "mem vs rd words", "mem vs wr cmds", "mem vs wr words", "mem loops"};
    printf("memcached debug (%s):", when);
    for (int k = 1; k < 25; k++) if (nm[k][0]) printf(" %s=%llu", nm[k], (unsigned long long)(v[k] & 0x00FFFFFFFFFFFFFFull));
    printf("\n"); fflush(stdout);
  };
  if (mcdk) { usleep(2000000); mcd_dump("2 s after mcd_mem start, before any traffic"); }
  // --plram: switch the alias on BEFORE any ring is touched -- every CPU and ConnectX access to BAR offsets [0, 512K)
  // from here on lands in PLRAM0/1/2
  volatile uint32_t *pa = nullptr;
  if (plram) {
    int afd = open(("/sys/bus/pci/devices/" + std::string(argv[1]) + "/resource0").c_str(), O_RDWR | O_SYNC);
    pa = (volatile uint32_t *)((uint8_t *)mmap(nullptr, 1 << 25, PROT_READ | PROT_WRITE, MAP_SHARED, afd, 0) + ALIAS_OFF);
    if (pa[0] != 0x50414C49) { printf("*** p2p_alias not found at BAR0+0x102000 -- is this card on fp7?\n"); return 1; }
    pa[1] = 0; usleep(1000); pa[2] = 0; pa[3] = 0; pa[8] = 0; pa[1] = 1; usleep(1000);
    if (pa[1] != 0x101) { printf("*** p2p_alias did not enable (CTRL 0x%x)\n", pa[1]); return 1; }
    printf("E5.6: p2p_alias ON, window = BAR offset 0 (512 KB) -> PLRAM0/1/2; rings, CQs, doorbell records, RX slots in PLRAM\n");
  }

  ibv_context *c0 = open_dev(argv[3], true);
  if (!c0) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
  ibv_pd *pd = ibv_alloc_pd(c0);
  // one MR over RX slots, the stop word and the TX buffer slots
  ibv_mr *dmr = ibv_reg_mr(pd, base + DATA_OFF, TXB_OFF + (STRIDE << LOG_TXS) - DATA_OFF, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *umem = mlx5dv_devx_umem_reg(c0, um, UMEM_LEN, IBV_ACCESS_LOCAL_WRITE);
  if (!dmr || !umem) { printf("registration failed: %s (is innova2_peermem loaded?)\n", strerror(errno)); return 1; }
  mlx5dv_pd dpd{}; mlx5dv_obj o{}; o.pd.in = pd; o.pd.out = &dpd; mlx5dv_init_obj(&o, MLX5DV_OBJ_PD);
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(c0, MLX5DV_UAR_ALLOC_TYPE_NC);
  memset(um, 0, UMEM_LEN);
  for (size_t so : {STOP_RX, STOP_TX, STOP_RP}) memset(base + so, 0, 64);
  for (unsigned i = 0; i < (1u << LOG_RCQ); i++) um[RCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_TCQ); i++) um[TCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_RQ); i++)
    mlx5dv_set_data_seg((mlx5_wqe_data_seg *)(um + RQ_OFF + i * 16), STRIDE, dmr->lkey, (uintptr_t)(base + DATA_OFF + i * STRIDE));
  _mm_sfence();
  e2_rx_cfg rcfg{dpd.pdn, uar->page_id, umem->umem_id, RCQ_OFF, RCQ_DBR, RQ_OFF, RQ_DBR, LOG_RCQ, LOG_RQ, 0x0800};
  rcfg.udp = 1; rcfg.dst_ip = ip; rcfg.udp_dport = port; rcfg.tag = 0x5100 | (port & 0xff);
  e2_rx rx;
  if (e2_rx_create(c0, &rcfg, &rx, err, sizeof(err))) { printf("*** RX: %s\n", err); return 1; }
  *(volatile uint32_t *)(um + RQ_DBR) = htobe32(1u << LOG_RQ);
  e3_cq tcq; e1_sq sq;
  if (e3_cq_create(c0, uar->page_id, umem->umem_id, TCQ_OFF, TCQ_DBR, LOG_TCQ, &tcq, err, sizeof(err))) { printf("*** TX CQ: %s\n", err); return 1; }
  int mi = e1_query_min_inline(c0, err, sizeof(err)); if (mi < 0) mi = 1;
  if (e1_create_sq(c0, dpd.pdn, tcq.cqn, uar->page_id, umem->umem_id, SQ_OFF, umem->umem_id, SQ_DBR, LOG_SQ, mi, &sq, err, sizeof(err))) { printf("*** SQ: %s\n", err); return 1; }
  _mm_sfence();

  const uint64_t uar_bus = bar_start(argv[4], 0) + (uint64_t)uar->page_id * 4096 + ((uint8_t *)uar->reg_addr - (uint8_t *)uar->base_addr);
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
  // step 1 option bits: fld_rx pre = npre | K << 8 | rxchk << 17 (adaptive K); fld_tx maxb bits 31:24 = cs_flags
  const unsigned pre = spec | (rxk << 8) | (rxchk << 17), maxb_s1 = maxb | (csum ? 0xC0u << 24 : 0u);
  xrt::run rrx(krx), rtx(ktx), rrp(krp);
  // kernel-side offsets: relative to each port's BO (--plram: the PLRAM BO holding that window range)
  const unsigned k2 = plram ? 0x40000u : 0u, k1 = plram ? 0x20000u : 0u;
  if (plram) { rrx.set_arg(0, pl2); rrx.set_arg(1, pl0); } else { rrx.set_arg(0, bo); rrx.set_arg(1, bo); }
  rrx.set_arg(2, rres);
  { int a = 4; for (unsigned v : {(unsigned)(UMEM_OFF + RCQ_OFF - k2), (unsigned)LOG_RCQ, (unsigned)(UMEM_OFF + RCQ_DBR), (unsigned)(UMEM_OFF + RQ_DBR),
                                  (unsigned)LOG_RQ, (unsigned)(DATA_OFF - k2), (unsigned)STRIDE, (unsigned)(STOP_RX - k2), 16u, pre, stamp}) rrx.set_arg(a++, v);
    rrx.set_arg(15, rlog); }
  rtx.set_arg(0, plram ? pl0 : bo); rtx.set_arg(1, bo); rtx.set_arg(2, pbo); rtx.set_arg(3, tres);
  { int a = 6;
    for (unsigned v : {(unsigned)(UMEM_OFF + SQ_OFF), (unsigned)LOG_SQ, (unsigned)(UMEM_OFF + SQ_DBR), sq.sqn, dmr->lkey, (unsigned)TXB_OFF}) rtx.set_arg(a++, v);
    rtx.set_arg(a++, (unsigned long long)(uintptr_t)(base + TXB_OFF));
    for (unsigned v : {(unsigned)LOG_TXS, (unsigned)STRIDE, db_off, bf, 256u, inl, sig, (unsigned)STOP_TX, stamp}) rtx.set_arg(a++, v);
    rtx.set_arg(22, tlog); rtx.set_arg(25, maxb_s1); }
  rrp.set_arg(0, plram ? pl1 : bo); rrp.set_arg(1, qres);
  { int a = 3; for (unsigned v : {(unsigned)(UMEM_OFF + TCQ_OFF - k1), (unsigned)LOG_TCQ, (unsigned)(UMEM_OFF + TCQ_DBR - k1), (unsigned)(STOP_RP - k1), gap}) rrp.set_arg(a++, v); }
  rrp.start(); rtx.start(); rrx.start();
  char ips[32]; in_addr ia{htonl(ip)}; inet_ntop(AF_INET, &ia, ips, sizeof(ips));
  printf("UDP echo endpoint UP: %s:%u on %s (tag 0x%x), rx checksum drop %s, tx cs_flags %s, pre 0x%x, maxb 0x%x -- %u s\n",
         ips, port, argv[3], rcfg.tag, rxchk ? "on" : "off", csum ? "L3|L4" : "off", pre, maxb_s1, secs_run);
  fflush(stdout);
  auto t0 = std::chrono::steady_clock::now();
  if (iperf) {
    // fns iperf_udp's runExperiment is level-sensitive (it restarts from IDLE while high): pulse it through iperf_ctl
    sleep(iperf_delay);
    xrt::kernel kc(dev, uuid, "iperf_ctl");
    auto pulse = [&](unsigned run) { xrt::run r(kc); r.set_arg(1, run); r.set_arg(2, iperf_words); r.set_arg(3, iperf_gap); r.set_arg(4, iperf_ip); r.start(); r.wait(); };
    pulse(1); usleep(500000); pulse(0);
    printf("iperf_udp started: %u x 64 B datagrams (%u B payload), gap %u cycles, target %s:5001 (from port 32768)\n",
           iperf_words, iperf_words * 64, iperf_gap, inet_ntoa(in_addr{iperf_ip})); fflush(stdout);
  }
  for (unsigned el = 0; std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs_run; el++) {
    sleep(1);
    if (stop_file && access(stop_file, F_OK) == 0) { printf("stop file %s seen -- ending the run\n", stop_file); break; }
    if (mcdk && mcd_dump_s && el % mcd_dump_s == mcd_dump_s - 1) {
      char w[64]; snprintf(w, sizeof w, "t=%ld", (long)time(nullptr)); mcd_dump(w);
    }
  }
  if (mcdk) mcd_dump("end of run, before stopping FLD-lite");
  for (size_t so : {STOP_RX, STOP_TX, STOP_RP}) *(volatile uint32_t *)(base + so) = 1;
  _mm_sfence();
  auto s1 = rrx.wait(std::chrono::milliseconds(10000)), s2 = rtx.wait(std::chrono::milliseconds(10000)), s3 = rrp.wait(std::chrono::milliseconds(10000));
  rres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); tres.sync(XCL_BO_SYNC_BO_FROM_DEVICE); qres.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  uint32_t r[16], t[16], q[16]; rres.read(r, sizeof(r), 0); tres.read(t, sizeof(t), 0); qres.read(q, sizeof(q), 0);
  auto dn = [](ert_cmd_state st) { return st == ERT_CMD_STATE_COMPLETED ? "done" : "NOT done"; };
  printf("fld_rx  (%s): CQEs %u, frames %u, RX drops %u (of which checksum/options %u), slot mismatches %u, polls %u\n", dn(s1), r[0], r[1], r[2], r[8], r[3], r[4]);
  printf("fld_tx  (%s): frames %u, WQEBBs %u, inline %u, buffered %u, credit waits %u, batches %u\n", dn(s2), t[0], t[1], t[2], t[3], t[4], t[6]);
  printf("fld_reap(%s): TX CQEs %u, error CQEs %u\n", dn(s3), q[0], q[1]);
  if (q[1]) printf("first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x\n", q[2] >> 28, (q[2] >> 16) & 0xFF, (q[2] >> 8) & 0xFF);
  printf("peer window: writes forwarded %u\n", pw[5] - w0);
  if (latrep) {
    // FPGA echo per frame from this card's own CQE timestamps (the E5.5 "port0 RX CQE -> TX CQE" measure): RX CQE j and
    // TX CQE j belong to the same frame when every WQE is signalled (--sig 1) and nothing was dropped; grouped by length
    ibv_device_attr_ex da{}; ibv_query_device_ex(c0, nullptr, &da);
    const double us = 1000.0 / (double)da.hca_core_clock;
    auto be = [&](size_t off, int nb) { uint64_t v = 0; for (int k = 0; k < nb; k++) v = (v << 8) | um[off + k]; return v; };
    std::map<unsigned, std::vector<double>> by;
    const unsigned nf = std::min<unsigned>(r[1], std::min(1u << LOG_RCQ, 1u << LOG_TCQ));
    unsigned bad = 0;
    for (unsigned j = 0; j < nf && r[2] == 0; j++) {
      const size_t rc = RCQ_OFF + (size_t)j * 64, tc = TCQ_OFF + (size_t)j * 64;
      const uint64_t a = be(rc + 48, 8) & 0x7fffffffffffffffull, b = be(tc + 48, 8) & 0x7fffffffffffffffull;
      const unsigned len = (unsigned)be(rc + 44, 4);
      if (!a || !b || b <= a) { bad++; continue; }
      by[len].push_back((b - a) * us);
    }
    printf("FPGA echo (this card's RX CQE -> TX CQE, %u frames%s):\n", nf, r[2] ? "; skipped: frames were dropped, indices would not pair" : "");
    for (auto &kv : by) { char nm[64]; snprintf(nm, sizeof nm, "frame %u B (%zu)", kv.first, kv.second.size()); st3(kv.second, nm); }
    if (bad) printf("  (%u pairs without usable timestamps)\n", bad);
  }
  if (pa) { printf("E5.6: p2p_alias redirected %u writes, %u reads; switching it off\n", pa[8], pa[9]); pa[1] = 0; usleep(1000); }
  const bool ok = s1 == ERT_CMD_STATE_COMPLETED && s2 == ERT_CMD_STATE_COMPLETED && s3 == ERT_CMD_STATE_COMPLETED && !q[1] && t[0] == r[1];
  printf("E5UDP-HOST %s (engines stopped cleanly, no TX error CQEs, every accepted frame echoed)\n", ok ? "OK" : "PROBLEM");
  (void)n; (void)stop_req;
  return ok ? 0 : 1;
}
