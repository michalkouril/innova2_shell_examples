// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Software simulation of the E5 FPGA traffic generator loop (2026-09-30): gen_tx and sink_rx (gen_sink.cpp) and the
// FLD-lite engines with the loopback AFU (fld_lite.cpp) run as threads against a fake two-port ConnectX:
//   port 1 TX follows gen_tx's SQ doorbell record, builds each frame from its WQE and "wires" it into port 0's RQ
//   (dropped, like the NIC, when no RQ WQE is posted); port 0 TX follows fld_tx's SQ and wires frames into port 1's RQ
//   (sink_rx). TX CQEs for CQ_UPDATE WQEs on both SQs; RX CQEs carry a timestamp.
//   usage: gen_sim [frames] [K] [batch] [gap] [inline_max] [bf] [log_sample]
#include "../fld_lite.cpp"
namespace gs {
#include "../gen/gen_sink.cpp"
}
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// bank0 region 1 (port 0 / FLD-lite), same layout as e5_host (rings in "URAM", RX slots at 0x80000)
// -DJUMBO: the e5gen_host --jumbo layout (16 KB slots, port-0 RX slots in DDR at 4 MB, TX slots 256 x 16 KB at 8 MB, 16 MB regions)
#ifdef JUMBO
static const unsigned B1 = (16u << 20) / 64, STRIDE = 16384, FSTR = 16384, LOG_TXS = 8, LOG_RQ = 8, G_LOG_RQ = 8;
static const unsigned DATA_OFF = 0x400000, TXB_OFF = 0x800000, G_FRM = 0x800000, G_EXP = 0xC00000;
#else
static const unsigned B1 = (8u << 20) / 64, STRIDE = 2048, FSTR = 2048, LOG_TXS = 10, LOG_RQ = 8, G_LOG_RQ = 10;
static const unsigned DATA_OFF = 0x80000, TXB_OFF = 0x400000, G_FRM = 0x400000, G_EXP = 0x600000;
#endif
static const unsigned LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11;
static const unsigned RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const unsigned SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000, STOP_OFF = 0x380000;
// region 2 (port 1 / generator), 8 MB
static const unsigned G_RQ = 0x0, G_RQ_DBR = 0x4000, G_RCQ_DBR = 0x4040, G_SQ_DBR = 0x4080, G_TCQ_DBR = 0x40C0;
static const unsigned G_SQ = 0x10000, G_RCQ = 0x20000, G_TCQ = 0x60000, G_TMPL = 0xA0000, G_ELEN = 0xA8000, G_STOP = 0xB0000;
static const unsigned G_DATA = 0x100000, G_LOG_RCQ = 12, G_LOG_SQ = 10, G_LOG_TCQ = 11;
static const unsigned long long VA1 = 0x7f0000000000ull, VA2 = 0x7e0000000000ull;

static beat_t *m1, *m2; static uint8_t *b1, *b2;
static beat_t peer[4096];
static std::atomic<bool> fail{false};
static uint32_t be32(uint8_t *m, unsigned o) { return (m[o] << 24) | (m[o + 1] << 16) | (m[o + 2] << 8) | m[o + 3]; }
static void wbe32(uint8_t *m, unsigned o, uint32_t v) { m[o] = v >> 24; m[o + 1] = v >> 16; m[o + 2] = v >> 8; m[o + 3] = v; }
static void fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
#ifdef JUMBO
static unsigned flen(unsigned k) { static const unsigned L[] = {60, 222, 1500, 1514, 2048, 2049, 4000, 8192, 8320, 8960, 9000, 777}; return L[k % 12]; }
#else
static unsigned flen(unsigned k) { static const unsigned L[] = {60, 64, 100, 128, 200, 222, 223, 256, 511, 1000, 1500, 777}; return L[k % 12]; }
#endif

// parse one WQE of an SQ ring (region m, ring base off, log size), return frame bytes in out; VA base maps pointers
static int parse_wqe(uint8_t *m, unsigned ring, unsigned log_sq, unsigned idx, unsigned long long va, uint8_t *mem_for_ptr,
                     uint8_t *out, unsigned *bbs_out, bool *sig)
{
  uint8_t wq[256];
  const uint8_t *w0 = m + ring + (idx & ((1u << log_sq) - 1)) * 64;
  const unsigned ds = w0[7] & 0x3f, bbs = (ds + 3) / 4;
  if (((w0[1] << 8) | w0[2]) != (idx & 0xffff) || w0[3] != 0x0a || w0[29] != 18) return -1;
  for (unsigned b = 0; b < bbs; b++) memcpy(wq + 64 * b, m + ring + ((idx + b) & ((1u << log_sq) - 1)) * 64, 64);
  memcpy(out, wq + 30, 18);
  unsigned olen = 18;
  const uint32_t bc = (wq[48] << 24) | (wq[49] << 16) | (wq[50] << 8) | wq[51];
  if (bc & 0x80000000u) { memcpy(out + 18, wq + 52, bc & 0x3ff); olen += bc & 0x3ff; }
  else { uint64_t a = 0; for (int k = 0; k < 8; k++) a = (a << 8) | wq[56 + k]; memcpy(out + 18, mem_for_ptr + (a - va), bc); olen += bc; }
  *bbs_out = bbs; *sig = wq[11] & 0x08;
  return olen;
}
// deliver a frame into an RQ/RCQ (ring in region m); returns false (dropped) if no RQ WQE is posted or the CQ is full
static bool deliver(uint8_t *m, unsigned rq_dbr, unsigned rcq, unsigned rcq_dbr, unsigned log_rq, unsigned log_rcq,
                    unsigned data, unsigned &cnt, const uint8_t *f, unsigned len, uint64_t ts)
{
  const unsigned posted = be32(m, rq_dbr) & 0xffff, ci = be32(m, rcq_dbr) & 0xffffff;
  if (((posted - cnt) & 0xffff) == 0 || cnt - ci >= (1u << log_rcq)) return false;
  const unsigned slot = cnt & ((1u << log_rq) - 1);
  memcpy(m + data + slot * STRIDE, f, len);
  uint8_t *c = m + rcq + (cnt & ((1u << log_rcq) - 1)) * 64;
  memset(c, 0, 63); wbe32(m, rcq + (cnt & ((1u << log_rcq) - 1)) * 64 + 44, len); c[60] = slot >> 8; c[61] = slot;
  for (int k = 0; k < 8; k++) c[48 + k] = ts >> (8 * (7 - k));
  fence(); c[63] = (0x2 << 4) | ((cnt >> log_rcq) & 1); fence();
  cnt++;
  return true;
}
static void tx_cqe(uint8_t *m, unsigned tcq, unsigned log_tcq, unsigned &cnt, unsigned idx)
{
  uint8_t *c = m + tcq + (cnt & ((1u << log_tcq) - 1)) * 64;
  memset(c, 0, 63); c[60] = idx >> 8; c[61] = idx; fence(); c[63] = (0x0 << 4) | ((cnt >> log_tcq) & 1); fence(); cnt++;
}

int main(int argc, char **argv)
{
  const unsigned n = argc > 1 ? atoi(argv[1]) : 20000, log_k = argc > 2 ? atoi(argv[2]) : 6, batch = argc > 3 ? atoi(argv[3]) : 16;
  const unsigned gap = argc > 4 ? atoi(argv[4]) : 0, inl = argc > 5 ? atoi(argv[5]) : 222, bf = argc > 6 ? atoi(argv[6]) : 1;
  const unsigned log_sample = argc > 7 ? atoi(argv[7]) : 0, K = 1u << log_k;
  m1 = new beat_t[B1]; b1 = (uint8_t *)m1; memset(b1, 0, (size_t)B1 * 64);
  m2 = new beat_t[B1]; b2 = (uint8_t *)m2; memset(b2, 0, (size_t)B1 * 64);
  for (unsigned i = 0; i < (1u << LOG_RCQ); i++) b1[RCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << LOG_TCQ); i++) b1[TCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << G_LOG_RCQ); i++) b2[G_RCQ + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < (1u << G_LOG_TCQ); i++) b2[G_TCQ + i * 64 + 63] = 0xF1;
  wbe32(b1, RQ_DBR, 1u << LOG_RQ); wbe32(b2, G_RQ_DBR, 1u << G_LOG_RQ);
  // templates: frame k (echo request, ET 0x88B6) at G_FRM + k*2048; WQE template; expected echo reply + length table
  uint16_t *el = (uint16_t *)(b2 + G_ELEN);
  for (unsigned k = 0; k < K; k++) {
    uint8_t *f = b2 + G_FRM + k * FSTR; const unsigned L = flen(k);
    for (unsigned q = 0; q < L; q++) f[q] = q * 13 + k * 5 + (q >> 8);
    memset(f, 0xFF, 6); f[12] = 0x88; f[13] = 0xB6; f[14] = k;
    uint8_t *e = b2 + G_EXP + k * FSTR; memcpy(e, f, L); e[6] = 0x02; e[11] = 0xEC; e[12] = 0x88; e[13] = 0xB8;
    el[k] = L;
    uint8_t *w = b2 + G_TMPL + k * 64; memset(w, 0, 64);
    w[7] = 4; w[29] = 18; memcpy(w + 30, f, 18);
    wbe32(b2, G_TMPL + k * 64 + 48, L - 18); wbe32(b2, G_TMPL + k * 64 + 52, 0x9999);
    const unsigned long long a = VA2 + G_FRM + k * FSTR + 18; for (int q = 0; q < 8; q++) w[56 + q] = a >> (8 * (7 - q));
  }
  auto P1 = [&](unsigned k) { return m1 + k; }; auto A1 = [&](unsigned off, unsigned k) { return off - 64 * k; };
  auto P2 = [&](unsigned k) { return m2 + k; }; auto A2 = [&](unsigned off, unsigned k) { return off - 64 * k; };
  static uint32_t rres[16], tres[16], qres[16], gres[16]; static ap_uint<64> sres[16]; static beat_t rlog[4096], tlog[4096];
  hls::stream<pkt_t> rxs("rxs"), txs("txs"); hls::stream<cred_t> cred("cred");
  hls::stream<req_t> q0("q0"), q1("q1"); hls::stream<tick_t> k0("k0"), k1("k1");
  std::thread([&] { e5_ticker2(q0, k0, q1, k1); }).detach();
  std::thread([&] { afu_loop(rxs, txs); }).detach();
  std::thread trx([&] { fld_rx(P1(1), P1(2), rres, rxs, A1(RCQ_OFF, 1), LOG_RCQ, A1(RCQ_DBR, 2), A1(RQ_DBR, 2), LOG_RQ,
                               A1(DATA_OFF, 1), STRIDE, A1(STOP_OFF, 1), 16, 2, 0, rlog, q0, k0); });
  std::thread ttx([&] { fld_tx(P1(3), P1(4), hls::burst_maxi<beat_t>(peer), tres, txs, cred, A1(SQ_OFF, 3), LOG_SQ, A1(SQ_DBR, 3),
                               0x1234, 0x5678, A1(TXB_OFF, 4), VA1 + TXB_OFF, LOG_TXS, STRIDE, 0x800, bf, 256, inl, 16,
                               A1(STOP_OFF, 3), 0, tlog, q1, k1, 16); });
  std::thread trp([&] { fld_reap(P1(5), qres, cred, A1(TCQ_OFF, 5), LOG_TCQ, A1(TCQ_DBR, 5), A1(STOP_OFF, 5), 8, A1(SQ_OFF, 5), getenv("NO_ZERO") ? 0 : LOG_SQ); });
  std::thread tgen([&] { gs::gen_tx(P2(1), hls::burst_maxi<beat_t>(peer + 64), gres, A2(G_TMPL, 1), log_k, A2(G_SQ, 1), G_LOG_SQ,
                                    A2(G_SQ_DBR, 1), 0x4321, A2(G_TCQ, 1), G_LOG_TCQ, A2(G_TCQ_DBR, 1), 0x800, n, batch, gap, 64,
                                    A2(G_STOP, 1), getenv("GEN_FLAGS") ? atoi(getenv("GEN_FLAGS")) : 0, 256); });
  std::thread tsnk([&] { gs::sink_rx(P2(2), P2(3), sres, A2(G_RCQ, 2), G_LOG_RCQ, A2(G_RCQ_DBR, 3), A2(G_RQ_DBR, 3), G_LOG_RQ,
                                     A2(G_DATA, 2), STRIDE, A2(G_EXP, 2), FSTR, A2(G_ELEN, 2), log_k, log_sample, n,
                                     A2(G_STOP, 2), 14); });
  // the fake NIC
  unsigned p1_ci = 0, p0_ci = 0, p0_rx = 0, p1_rx = 0, p1_tcq = 0, p0_tcq = 0, drop0 = 0, drop1 = 0;
  uint64_t ts = 1000;
  static uint8_t fr[16384];
  auto t0 = std::chrono::steady_clock::now();
  while (!fail) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { printf("TIMEOUT\n"); fail = true; break; }
    fence();
    bool did = false;
    // port 1 TX (generator) -> port 0 RX
    const unsigned g_pi = be32(b2, G_SQ_DBR + 4) & 0xffff;
    if (p1_ci != g_pi && p1_tcq - (be32(b2, G_TCQ_DBR) & 0xffffff) < (1u << G_LOG_TCQ)) {
      unsigned bbs; bool sig;
      const int len = parse_wqe(b2, G_SQ, G_LOG_SQ, p1_ci, VA2, b2, fr, &bbs, &sig);
      if (len < 0) { printf("FAIL: bad generator WQE at %u\n", p1_ci); fail = true; break; }
      if (getenv("GENSIM_TRACE")) printf("trace p1->p0 len %d\n", len);
      if (!deliver(b1, RQ_DBR, RCQ_OFF, RCQ_DBR, LOG_RQ, LOG_RCQ, DATA_OFF, p0_rx, fr, len, ts++)) drop0++;
      if (sig) tx_cqe(b2, G_TCQ, G_LOG_TCQ, p1_tcq, p1_ci);
      p1_ci = (p1_ci + bbs) & 0xffff; did = true;
    }
    // port 0 TX (FLD-lite) -> port 1 RX (sink)
    const unsigned f_pi = be32(b1, SQ_DBR + 4) & 0xffff;
    if (p0_ci != f_pi && p0_tcq - (be32(b1, TCQ_DBR) & 0xffffff) < (1u << LOG_TCQ)) {
      unsigned bbs; bool sig;
      const int len = parse_wqe(b1, SQ_OFF, LOG_SQ, p0_ci, VA1, b1, fr, &bbs, &sig);
      if (len < 0) { printf("FAIL: bad FLD WQE at %u\n", p0_ci); fail = true; break; }
      if (getenv("GENSIM_TRACE")) printf("trace p0->p1 len %d\n", len);
      if (!deliver(b2, G_RQ_DBR, G_RCQ, G_RCQ_DBR, G_LOG_RQ, G_LOG_RCQ, G_DATA, p1_rx, fr, len, ts++)) drop1++;
      if (sig) tx_cqe(b1, TCQ_OFF, LOG_TCQ, p0_tcq, p0_ci);
      p0_ci = (p0_ci + bbs) & 0xffff; did = true;
    }
    if (!did && p1_rx + drop0 + drop1 >= n) break;
    if (!did) std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  wbe32(b1, STOP_OFF, 1); wbe32(b2, G_STOP, 1); fence();
  tgen.join(); tsnk.join(); trx.join(); ttx.join(); trp.join();
  printf("gen_tx : posted %u in %u batches, TX CQEs %u, credit polls %u\n", gres[0], gres[1], gres[2], gres[5]);
  printf("nic    : port0 RX %u (dropped %u), port1 RX %u (dropped %u)\n", p0_rx, drop0, p1_rx, drop1);
  printf("fld    : rx frames %u, tx frames %u, batches %u\n", rres[1], tres[0], tres[6]);
  printf("sink_rx: frames %llu, bytes %llu, errors %llu, checked %llu, mismatches %llu (first at %lld), length mismatches %llu\n",
         (unsigned long long)sres[0], (unsigned long long)sres[1], (unsigned long long)sres[2], (unsigned long long)sres[3],
         (unsigned long long)sres[4], (long long)(int)(unsigned)sres[8], (unsigned long long)sres[9]);
  // loss is allowed when the generator outruns FLD-lite (the NIC drops on an empty RQ, as the real one does); what must
  // hold: every frame accounted for, nothing corrupted, nothing lost after port 0
  const bool pass = !fail && gres[0] == n && sres[0] + drop0 == n && drop1 == 0 && sres[4] == 0 && sres[9] == 0 && sres[3] > 0;
  printf("loss before FLD-lite (port 0 RQ empty): %u of %u\n", drop0, n);
  printf("GENSIM %s\n", pass ? "PASS" : "FAIL"); fflush(stdout); _Exit(pass ? 0 : 1);
}
