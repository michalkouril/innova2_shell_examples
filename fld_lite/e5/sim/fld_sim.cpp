// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.1 software simulation (2026-09-30): the four FLD-lite kernels (fld_lite.cpp) run as threads against a fake
// ConnectX that uses the same memory layout, ring formats and ownership rules as the real one:
//   RX: frames land in RQ slots (only while the RQ doorbell record says a WQE is posted), then a CQE (byte_cnt @44,
//       wqe_counter @60, op_own @63) is written with the owner byte last; the CQ is never overrun (set_ci).
//   TX: the NIC follows the SQ doorbell record, parses each WQE (ctrl, eth seg with 18-byte inline header, inline or
//       pointer data segment), rebuilds the frame and checks it byte-for-byte against the expected echo, and writes a
//       TX CQE (wqe_counter = WQE index) for every WQE with CQ_UPDATE.
//   usage: fld_sim [frames] [inline_max] [bf] [sig_every] [error_every] [pre] [stamp] [gap] [maxb]
#include "../fld_lite.cpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static const unsigned BEATS = (8u << 20) / 64;                      // 8 MB bank0 buffer
static const unsigned LOG_RQ = 10, LOG_RCQ = 11, LOG_SQ = 10, LOG_TCQ = 11, STRIDE = 2048;
static const unsigned RQ_DBR = 0x4000, RCQ_DBR = 0x4040, SQ_DBR = 0x4080, TCQ_DBR = 0x40C0;
static const unsigned SQ_OFF = 0x10000, RCQ_OFF = 0x20000, TCQ_OFF = 0x40000;
static const unsigned DATA_OFF = 0x100000, TXB_OFF = 0x400000, LOG_TXS = 10, STOP_OFF = 0x380000;
static const unsigned long long VA = 0x7f0000000000ull;            // the "virtual address" of bank0 offset 0
static const unsigned DB_OFF = 0x800, BF_SIZE = 256;

static beat_t *mem;
static uint8_t *m8;
static beat_t peer[4096];
static std::atomic<bool> fail{false};

static uint32_t be32(unsigned off) { const uint8_t *p = m8 + off; return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void wbe32(unsigned off, uint32_t v) { uint8_t *p = m8 + off; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
static unsigned flen(unsigned i) { static const unsigned L[] = {60, 64, 100, 128, 200, 222, 223, 256, 511, 1000, 1500, 777}; return L[i % 12]; }
static uint8_t fbyte(unsigned i, unsigned k) { return (uint8_t)(k * 13 + i * 5 + (k >> 8)); }
static void make_frame(uint8_t *f, unsigned i, unsigned len)
{
  for (unsigned k = 0; k < len; k++) f[k] = fbyte(i, k);
  memset(f, 0xFF, 6); f[12] = 0x88; f[13] = 0xB6; f[14] = i; f[15] = i >> 8;
}

int main(int argc, char **argv)
{
  const unsigned n = argc > 1 ? atoi(argv[1]) : 5000, inl = argc > 2 ? atoi(argv[2]) : 222, bf = argc > 3 ? atoi(argv[3]) : 1;
  const unsigned sig = argc > 4 ? atoi(argv[4]) : 1, err_every = argc > 5 ? atoi(argv[5]) : 0;
  const unsigned maxb = argc > 9 ? atoi(argv[9]) : 16;
  const unsigned spec = argc > 6 ? atoi(argv[6]) : 4, stamp = argc > 7 ? atoi(argv[7]) : 0, gap = argc > 8 ? atoi(argv[8]) : 0;
  static beat_t rlog[4096], tlog[4096];
  mem = new beat_t[BEATS]; m8 = reinterpret_cast<uint8_t *>(mem); memset(m8, 0, (size_t)BEATS * 64);
  const unsigned rcq_n = 1u << LOG_RCQ, tcq_n = 1u << LOG_TCQ, rq_n = 1u << LOG_RQ;
  for (unsigned i = 0; i < rcq_n; i++) m8[RCQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < tcq_n; i++) m8[TCQ_OFF + i * 64 + 63] = 0xF1;
  wbe32(RQ_DBR, rq_n);

  // each kernel port gets its own base pointer (K beats in) so no two ports share bookkeeping; its args are shifted
  // back by the same amount (every port's address args are disjoint from the other ports' in the same kernel)
  auto P = [&](unsigned k) { return mem + k; };
  auto A = [&](unsigned off, unsigned k) { return off - 64 * k; };
  static uint32_t rres[16], tres[16], qres[16];
  hls::stream<pkt_t> rxs("rxs"), txs("txs");
  hls::stream<cred_t> cred("cred");
  hls::stream<req_t> q0("q0"), q1("q1");
  hls::stream<tick_t> k0("k0"), k1("k1");
  std::thread ttk([&] { e5_ticker2(q0, k0, q1, k1); });
  ttk.detach();
  std::thread trx([&] { fld_rx(P(1), P(2), rres, rxs, A(RCQ_OFF, 1), LOG_RCQ, A(RCQ_DBR, 2), A(RQ_DBR, 2), LOG_RQ,
                               A(DATA_OFF, 1), STRIDE, A(STOP_OFF, 1), 16, spec, stamp, rlog, q0, k0); });
  std::thread tafu([&] { afu_loop(rxs, txs); });
  tafu.detach();
  std::thread ttx([&] { fld_tx(P(3), P(4), hls::burst_maxi<beat_t>(peer), tres, txs, cred, A(SQ_OFF, 3), LOG_SQ,
                               A(SQ_DBR, 3), 0x1234, 0x5678, A(TXB_OFF, 4), VA + TXB_OFF, LOG_TXS, STRIDE, DB_OFF, bf,
                               BF_SIZE, inl, sig, A(STOP_OFF, 3), stamp, tlog, q1, k1, maxb); });
  std::thread trp([&] { fld_reap(P(5), qres, cred, A(TCQ_OFF, 5), LOG_TCQ, A(TCQ_DBR, 5), A(STOP_OFF, 5), gap, A(SQ_OFF, 5), 0); });

  // ---- the fake NIC: RX producer and TX consumer interleaved in one thread ----
  unsigned sent = 0, rx_cqe = 0, echoed = 0, good = 0, tx_cqe = 0, nic_ci = 0, errs_injected = 0, wqes = 0;
  std::vector<unsigned> expect_ids;                                 // frame ids in the order their echoes must appear
  auto t0 = std::chrono::steady_clock::now();
  while (echoed + errs_injected < n && !fail) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) { printf("TIMEOUT\n"); fail = true; break; }
    fence();
    // RX: next frame if an RQ WQE is posted and the RX CQ has room
    const unsigned posted = be32(RQ_DBR) & 0xffff, rcq_ci = be32(RCQ_DBR) & 0xffffff;
    if (sent < n && ((posted - rx_cqe) & 0xffff) > 0 && rx_cqe - rcq_ci < rcq_n) {
      const unsigned slot = rx_cqe & (rq_n - 1), len = flen(sent);
      const bool err = err_every && (sent % err_every) == err_every - 1;
      if (!err) make_frame(m8 + DATA_OFF + slot * STRIDE, sent, len);
      uint8_t *c = m8 + RCQ_OFF + (rx_cqe & (rcq_n - 1)) * 64;
      memset(c, 0, 63); wbe32(RCQ_OFF + (rx_cqe & (rcq_n - 1)) * 64 + 44, len); c[60] = slot >> 8; c[61] = slot;
      fence();
      c[63] = ((err ? 0xD : 0x2) << 4) | ((rx_cqe >> LOG_RCQ) & 1);   // owner byte last
      fence();
      if (err) errs_injected++; else expect_ids.push_back(sent);
      sent++; rx_cqe++;
    }
    // TX: consume WQEs up to the SQ doorbell record
    const unsigned spi = be32(SQ_DBR + 4) & 0xffff, tcq_ci = be32(TCQ_DBR) & 0xffffff;
    while (nic_ci != spi && tx_cqe - tcq_ci < tcq_n && !fail) {
      const uint8_t *w = m8 + SQ_OFF + (nic_ci & ((1u << LOG_SQ) - 1)) * 64;
      const unsigned idx = (w[1] << 8) | w[2], opc = w[3], ds = w[7] & 0x3f, flags = w[11];
      if (idx != nic_ci || opc != 0x0a || ((w[4] << 16) | (w[5] << 8) | w[6]) != 0x1234 || w[29] != 18) {
        printf("FAIL: bad WQE at %u: idx %u opc 0x%x ds %u\n", nic_ci, idx, opc, ds); fail = true; break;
      }
      const unsigned bbs = (ds + 3) / 4;
      // gather the WQE (it may wrap the ring)
      uint8_t wq[256];
      for (unsigned b = 0; b < bbs; b++) memcpy(wq + 64 * b, m8 + SQ_OFF + ((nic_ci + b) & ((1u << LOG_SQ) - 1)) * 64, 64);
      uint8_t out[2048]; unsigned olen = 18;
      memcpy(out, wq + 30, 18);
      const uint32_t bc = (wq[48] << 24) | (wq[49] << 16) | (wq[50] << 8) | wq[51];
      if (bc & 0x80000000u) { memcpy(out + 18, wq + 52, bc & 0x3ff); olen += bc & 0x3ff; }
      else {
        const uint32_t lkey = (wq[52] << 24) | (wq[53] << 16) | (wq[54] << 8) | wq[55];
        uint64_t a = 0; for (int k = 0; k < 8; k++) a = (a << 8) | wq[56 + k];
        if (lkey != 0x5678 || a < VA + TXB_OFF || a + bc > VA + TXB_OFF + (STRIDE << LOG_TXS)) { printf("FAIL: bad data seg lkey 0x%x addr 0x%llx\n", lkey, (unsigned long long)a); fail = true; break; }
        memcpy(out + 18, m8 + (a - VA), bc); olen += bc;
      }
      if (echoed >= expect_ids.size()) { printf("FAIL: echo %u with no frame received for it\n", echoed); fail = true; break; }
      const unsigned id = expect_ids[echoed];
      uint8_t ref[2048]; make_frame(ref, id, flen(id)); ref[6] = 0x02; ref[11] = 0xEC; ref[12] = 0x88; ref[13] = 0xB8;
      const bool ok = olen == flen(id) && !memcmp(out, ref, olen);
      if (!ok && good == echoed) printf("first mismatch: echo %u (frame %u) len %u want %u\n", echoed, id, olen, flen(id));
      good += ok; echoed++; wqes++;
      if (flags & 0x08) {
        uint8_t *c = m8 + TCQ_OFF + (tx_cqe & (tcq_n - 1)) * 64;
        memset(c, 0, 63); c[60] = nic_ci >> 8; c[61] = nic_ci; fence();
        c[63] = (0x0 << 4) | ((tx_cqe >> LOG_TCQ) & 1); fence();
        tx_cqe++;
      }
      nic_ci = (nic_ci + bbs) & 0xffff;
    }
  }
  // let the kernels drain, then stop them
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  wbe32(STOP_OFF, 1); m8[STOP_OFF + 3] = 1; fence();
  trx.join(); ttx.join(); trp.join();
  printf("fld_rx : CQEs %u frames %u errors %u slot-mismatch %u polls %u multi %u maxrun %u kpolls %u\n", rres[0], rres[1], rres[2], rres[3], rres[4], rres[5], rres[6], rres[7]);
  printf("fld_tx : frames %u WQEBBs %u inline %u buffered %u credit-waits %u batches %u lingered %u biggest %u carries %u\n", tres[0], tres[1], tres[2], tres[3], tres[4], tres[6], tres[7], tres[8], tres[9]);
  printf("fld_reap: CQEs %u errors %u\n", qres[0], qres[1]);
  printf("nic    : sent %u (%u error CQEs), echoed %u, byte-exact %u, TX CQEs %u (sig_every %u, inline <= %u, bf %u)\n",
         sent, errs_injected, echoed, good, tx_cqe, sig, inl, bf);
  if (stamp) {                                                       // the logs must be complete and time-ordered
    unsigned bad = 0, m = std::min(echoed, 4096u);
    for (unsigned i = 0; i < m; i++) {
      const beat_t &a = rlog[i], &b = tlog[i];
      const uint64_t t0 = a.range(63, 0), t1 = a.range(127, 64), s0 = b.range(63, 0), s1 = b.range(127, 64), s2 = b.range(191, 128), s3 = b.range(255, 192);
      if (!(t0 && t0 <= t1 && t1 <= s0 && s0 <= s1 && s1 <= s2 && s2 <= s3) || (unsigned)b.range(287, 256) != i) bad++;
    }
    printf("stamps : %u of %u records out of order or missing\n", bad, m);
    if (bad) fail = true;
  }
  const bool pass = !fail && echoed == n - errs_injected && good == echoed && rres[2] == errs_injected && tres[0] == echoed;
  printf("SIM %s\n", pass ? "PASS" : "FAIL");
  fflush(stdout); _Exit(pass ? 0 : 1);                                              // afu_loop never returns
}
