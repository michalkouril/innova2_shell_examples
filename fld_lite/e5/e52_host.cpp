// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.2 host (2026-09-30): the FLD-lite engines with the FlexDriver ZUC AFU in place of the loopback AFU:
//   fld_rx -> zuc_in -> zuc_k (RTL) -> zuc_out -> fld_tx      (no CPU in the data path)
// Port 1 sends ZUC requests (Ethernet mode: Eth/IPv4/UDP beat, ZUC header beat, payload); every reply is matched by
// its echoed 64-bit message ID and checked against the software model (zuc/zuc_model.h): CONF ciphertext
// byte-for-byte, INTEG MAC, swapped L2-L4 addresses, zeroed checksums, IP/UDP lengths.
//     --op conf|integ|mix   (default mix)    --len L payload bytes 1..1372 (default: cycle 1..1372)
//     --guard               interleave malformed requests (len 0, runt, opcode 4, beat mismatch): must be dropped
//     --lat / --burst / --bf / --inline N / --sig K / --stamp / --pre N / --gap N / --maxb N  as e5_host
// Queue setup as E3/E5.1 (rings in the bank0 URAM window, frames in URAM/DDR, doorbells through PEER).
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
#include <xrt/experimental/xrt_ip.h>
#include "zuc/zuc_model.h"

static const size_t BUF = 8 << 20, STRIDE = 2048, RXLEN = 2048;
static int LOG_RQ = 8; static const int LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11, LOG_TXS = 10;
static size_t DATA_OFF = 0x80000;                         // RX slots in the URAM window (E5.1c default)
static const size_t UMEM_OFF = 0x0, UMEM_LEN = 0x60000, TXB_OFF = 0x400000, STOP_OFF = 0x380000;
static const size_t RQ_OFF = 0x0, RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const size_t SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const uint16_t ET_IN = 0x88B6, ET_OUT = 0x88B8;
static const size_t PW_OFF = 0x100000;

enum { OP_CONF = 0, OP_INTEG = 1 };
static int OPMODE = -1;                                            // -1 mix, else OP_CONF / OP_INTEG
static unsigned PLEN = 0;                                          // 0: cycle through LENS
static bool GUARD = false;
static const unsigned LENS[] = {1, 13, 64, 65, 100, 128, 200, 511, 1000, 1372};
struct zreq { unsigned op, len, kind; };                           // kind 0 valid; 1 len 0; 2 runt; 3 opcode 4; 4 beat mismatch
static zreq req_of(unsigned i)
{
  zreq r;
  r.op = OPMODE >= 0 ? (unsigned)OPMODE : (i & 1);
  r.len = PLEN ? PLEN : LENS[(i / 2) % 10];
  r.kind = GUARD && (i % 5) == 4 ? 1 + (i / 5) % 4 : 0;
  return r;
}
static unsigned rng(unsigned i, unsigned k) { unsigned x = i * 2654435761u ^ (k * 40503u + 0x9e3779b9u); x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; return x; }
static void keyiv(unsigned i, uint8_t *key, uint8_t *iv) { for (int k = 0; k < 16; k++) { key[k] = rng(i, k); iv[k] = rng(i, 100 + k); } }
static void payload(unsigned i, uint8_t *p, unsigned len) { for (unsigned k = 0; k < len; k++) p[k] = rng(i, 1000 + k); }
static uint16_t ipcsum(const uint8_t *h) { uint32_t s = 0; for (int k = 0; k < 20; k += 2) s += (h[k] << 8) | h[k + 1]; s = (s & 0xffff) + (s >> 16); s = (s & 0xffff) + (s >> 16); return ~s & 0xffff; }
// builds request i into f, returns its frame length
static unsigned make_req(uint8_t *f, unsigned i)
{
  const zreq r = req_of(i);
  const unsigned L = r.kind == 1 ? 0 : r.kind == 4 ? 300 : r.len;
  const unsigned flen = r.kind == 2 ? 64 : r.kind == 4 ? 192 : 128 + L;
  memset(f, 0, 2048);
  memcpy(f, "\x02\x49\x4e\x4e\x32\x00", 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF;
  uint8_t *ip = f + 14;                                            // IPv4: len, id, ttl 64, UDP, 10.0.0.2 -> 10.0.0.1
  ip[0] = 0x45; ip[2] = (flen - 14) >> 8; ip[3] = flen - 14; ip[4] = i >> 8; ip[5] = i; ip[6] = 0x40; ip[8] = 64; ip[9] = 17;
  ip[12] = 10; ip[15] = 2; ip[16] = 10; ip[19] = 1;
  const uint16_t ck = ipcsum(ip); ip[10] = ck >> 8; ip[11] = ck;   // non-zero: only processed replies carry 0
  uint8_t *u = f + 34; u[0] = 0x30; u[1] = 0x39; u[2] = 0x16; u[3] = 0x2e; u[4] = (flen - 34) >> 8; u[5] = flen - 34;
  for (int k = 42; k < 64; k++) f[k] = 0xAA;
  if (flen > 64) {
    uint8_t *h = f + 64;
    h[0] = r.kind == 3 ? 4 : r.op; h[1] = 0x5A; h[2] = L >> 8; h[3] = L;
    for (int k = 0; k < 8; k++) h[4 + k] = (uint64_t)i >> (8 * (7 - k));   // message id = request index
    keyiv(i, h + 12, h + 28);
    for (int k = 44; k < 64; k++) h[k] = 0xCC;
    if (flen > 128) payload(i, f + 128, flen - 128);
  }
  return flen;
}
// checks a reply against the model; returns 0 if exact, else a short reason
static const char *check_reply(const uint8_t *f, unsigned len, unsigned &id)
{
  id = 0; for (int k = 0; k < 8; k++) id = (id << 8) | f[64 + 4 + k];
  const zreq r = req_of(id);
  if (r.kind) return "reply to a malformed request";
  uint8_t q[2048]; const unsigned qlen = make_req(q, id);
  const unsigned L = r.len, want = r.op == OP_CONF ? 128 + L : 112;
  if (len != want) return "length";
  if (memcmp(f, q + 6, 6) || memcmp(f + 6, q, 6) || f[12] != q[12] || f[13] != q[13]) return "MAC swap";
  if (memcmp(f + 26, q + 30, 4) || memcmp(f + 30, q + 26, 4) || memcmp(f + 34, q + 36, 2) || memcmp(f + 36, q + 34, 2)) return "IP/UDP swap";
  if (f[24] || f[25] || f[40] || f[41]) return "checksums not zero";
  const unsigned iplen = (f[16] << 8) | f[17], udplen = (f[38] << 8) | f[39];
  // CONF replies carry the ciphertext: IP length len + 0x72, UDP len + 0x5E. An INTEG reply has no payload (112-byte
  // frame), so its lengths are constants: IP 0x62 (98), UDP 0x4E (78).
  const unsigned want_ip = r.op == OP_CONF ? L + 0x72u : 0x62u, want_udp = r.op == OP_CONF ? L + 0x5Eu : 0x4Eu;
  if (iplen != want_ip || udplen != want_udp) return "IP/UDP length";
  if (memcmp(f + 64, q + 64, 12)) return "header echo";
  uint8_t key[16], iv[16]; keyiv(id, key, iv);
  if (r.op == OP_CONF) {
    uint8_t ct[2048]; zuc_xor(key, iv, q + 128, ct, L);
    for (int k = 76; k < 128; k++) if (f[k]) return "header not cleared";
    if (memcmp(f + 128, ct, L)) return "CIPHERTEXT";
  } else {
    const uint32_t mac = eia3_mac(key, iv, q + 128, 8 * L);
    const uint32_t got = (f[108] << 24) | (f[109] << 16) | (f[110] << 8) | f[111];
    if (got != mac) return "MAC";
  }
  (void)qlen;
  return nullptr;
}
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
    if (!strcmp(argv[i], "--bf")) bf = 1;
    if (!strcmp(argv[i], "--inline") && i + 1 < argc) inl = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--len") && i + 1 < argc) PLEN = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--sig") && i + 1 < argc) sig = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--stamp")) stamp = 1;
    if (!strcmp(argv[i], "--op") && i + 1 < argc) OPMODE = !strcmp(argv[i + 1], "conf") ? OP_CONF : !strcmp(argv[i + 1], "integ") ? OP_INTEG : -1;
    if (!strcmp(argv[i], "--guard")) GUARD = true;
    if (!strcmp(argv[i], "--rxddr")) { DATA_OFF = 0x100000; LOG_RQ = 10; }
    if (!strcmp(argv[i], "--pre") && i + 1 < argc) spec = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--maxb") && i + 1 < argc) maxb = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atoi(argv[i + 1]);
  }
  if (PLEN > 1372) { fprintf(stderr, "--len must be 1..1372 (one Ethernet frame)\n"); return 2; }
  if (zuc_selftest()) { fprintf(stderr, "software ZUC model failed its self-test\n"); return 1; }
  if (inl > 222) inl = 222;
  if (lat) sig = 1;
  if (sig < 1) sig = 1;
  if (n > 1000000) n = 1000000;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krx(dev, uuid, "fld_rx"), ktx(dev, uuid, "fld_tx"), krp(dev, uuid, "fld_reap");
  // the ZUC AFU's CSRs (user-managed kernel): scratchpad round trip, clock from its timestamp counter
  xrt::ip zip(dev, uuid, "zuc_k:{zuc_k_1}");
  auto zip_report = [&](bool end) {
    if (!end) {
      zip.write_register(0x1030, 0x5a17c0de);
      const uint32_t sp = zip.read_register(0x1030);
      const uint64_t a = zip.read_register(0x1200) | ((uint64_t)zip.read_register(0x1204) << 32);
      usleep(100000);
      const uint64_t b = zip.read_register(0x1200) | ((uint64_t)zip.read_register(0x1204) << 32);
      printf("zuc_k CSRs: scratchpad 0x%08x (%s), AFU clock ~%.1f MHz from its timestamp counter\n", sp, sp == 0x5a17c0de ? "ok" : "WRONG",
             (b - a) / 1e5);
    } else {
      printf("zuc_k CSRs: status 0x%08x, RX backpressure clocks %u, TX backpressure clocks %u (must stay 0 behind the safe FIFO)\n",
             zip.read_register(0x1104), zip.read_register(0x1120), zip.read_register(0x1124));
      printf("zuc_k counters: C0 buffers 0x%08x  C2 fifo_in/out 0x%08x  C3 fifo_out cnt(3..0) 0x%08x  C4 (7..4) 0x%08x  "
             "C6 state machines 0x%08x  C7 module states 0x%08x\n", zip.read_register(0x1100), zip.read_register(0x1108),
             zip.read_register(0x110C), zip.read_register(0x1110), zip.read_register(0x1118), zip.read_register(0x111C));
    }
  };
  zip_report(false);
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
  printf("E5.2 ZUC: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%llx via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, ops %s\n",
         rx.rqn, rx.cqn, sq.sqn, tcq.cqn, (unsigned long long)uar_bus, db_off, bf ? "on" : "off", inl, sig,
         OPMODE < 0 ? "CONF+INTEG" : OPMODE == OP_CONF ? "CONF" : "INTEG");

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
  // (the engines are started only after port 1 is fully set up: an early exit must not orphan running kernels)

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
  fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(ET_IN); fl.e.mask.ether_type = 0xFFFF;   // ZUC replies keep the request ethertype
  ibv_flow *flow = ibv_create_flow(eqp, &fl.a);
  if (!flow) { perror("create_flow"); return 1; }
  auto post_rx = [&](unsigned slot) {
    ibv_sge s{(uint64_t)(rxb + slot * RXLEN), (uint32_t)RXLEN, rmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = slot; w.sg_list = &s; w.num_sge = 1;
    ibv_post_recv(eqp, &w, &bad);
  };
  for (unsigned i = 0; i < 1024; i++) post_rx(i);
  rrp.start(); rtx.start(); rrx.start();

  unsigned sent = 0, inflight = 0, got = 0, good = 0, rxerr = 0, ooo = 0, next_id = 0, nvalid = 0, bad_shown = 0;
  unsigned nconf = 0, ninteg = 0;
  for (unsigned i = 0; i < n; i++) { const zreq r = req_of(i); if (!r.kind) { nvalid++; (r.op == OP_CONF ? nconf : ninteg)++; } }
  std::vector<unsigned> seen(n, 0);
  auto drain = [&]() {
    while (poll_ts(ecqx, [&](uint64_t wr, ibv_wc_status stt, uint32_t len, uint64_t t) {
        unsigned slot = (unsigned)wr;
        if (stt) rxerr++;
        else {
          unsigned id; const char *why = check_reply(rxb + slot * RXLEN, len, id);
          if (id < n) { seen[id]++; ts_rx1[id] = t; }
          if (!why && id < n && seen[id] == 1) good++;
          else if (bad_shown++ < 5) printf("reply %u (len %u): %s\n", id, len, why ? why : "duplicate");
          ooo += id < next_id; next_id = id + 1;
        }
        got++; post_rx(slot);
      }) > 0) {}
  };
  auto nreplied_upto = [&](unsigned k) { unsigned c = 0; for (unsigned i = 0; i < k; i++) c += !req_of(i).kind; return c; };
  auto t0 = std::chrono::steady_clock::now();
  std::vector<double> rtt;
  while (sent < n) {
    auto ts = std::chrono::steady_clock::now();
    unsigned s = sent % 64; const unsigned fl = make_req(tx + s * RXLEN, sent);
    ibv_sge sg{(uint64_t)(tx + s * RXLEN), fl, tmr->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = sent; w.sg_list = &sg; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(sqp, &w, &bad)) { perror("post_send"); break; }
    sent++; inflight++;
    int k; do { k = poll_ts(scqx, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { if (wr < n) ts_tx1[wr] = t; }); inflight -= k; } while (inflight >= 60);
    drain();
    if (lat) {
      const bool expect = !req_of(sent - 1).kind;
      while (expect && got < nreplied_upto(sent) && std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count() < 0.2) drain();
      if (expect && got == nreplied_upto(sent)) rtt.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - ts).count());
    } else if (!burst) usleep(2);
  }
  for (int t = 0; t < 2000 && got < nvalid; t++) { drain(); usleep(1000); }
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
  printf("port 1: sent %u requests (%u valid: %u CONF, %u INTEG; %u malformed), replies %u, CORRECT %u, out of order %u, RX errors %u, missing %d  (%.3f s)\n",
         sent, nvalid, nconf, ninteg, sent - nvalid, got, good, ooo, rxerr, (int)nvalid - (int)got, secs);
  if (!lat && got > 1) {
    uint64_t a = ts_tx1[0], b = 0; for (unsigned i = 0; i < n; i++) b = std::max(b, ts_rx1[i]);
    if (a && b > a) {
      double d = (b - a) * us * 1e-6; double bytes = 0; for (unsigned i = 0; i < n; i++) if (!req_of(i).kind) bytes += req_of(i).len;
      printf("rate (ConnectX clock, first send -> last reply): %.0f requests/s, %.2f Gb/s of ZUC payload\n", got / d, bytes * 8 / d * 1e-9);
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
  zip_report(true);
  const bool pass = got == nvalid && good == nvalid && !rxerr && r[1] == n && t[0] == nvalid && !r[2] && !q[1] &&
                    s1 == ERT_CMD_STATE_COMPLETED && s2 == ERT_CMD_STATE_COMPLETED && s3 == ERT_CMD_STATE_COMPLETED;
  printf("RESULT %s (E5.2 ZUC: fld_rx -> zuc_in -> zuc_k -> zuc_out -> fld_tx, no CPU in the data path)\n", pass ? "PASS" : "FAIL");
  pw[1] = 0;
  ibv_destroy_flow(flow); ibv_destroy_qp(eqp); ibv_destroy_qp(sqp); ibv_destroy_cq(ecq); ibv_destroy_cq(scq);
  ibv_dereg_mr(rmr); ibv_dereg_mr(tmr); ibv_dealloc_pd(p1);
  e1_destroy_sq(&sq); e3_cq_destroy(&tcq); e2_rx_destroy(&rx); mlx5dv_devx_free_uar(uar); mlx5dv_devx_umem_dereg(umem);
  ibv_dereg_mr(dmr); ibv_dealloc_pd(pd); ibv_close_device(c0); ibv_close_device(c1);
  return pass ? 0 : 1;
}
