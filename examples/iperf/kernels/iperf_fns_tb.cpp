// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// csim for iperf_fns: fns iperf_udp client through our wrapper -- datagram metadata, word counts, iperf sequence
// numbers (big-endian int32 at payload byte 0, counting up from 0, the last one negative), then the iperf2 server's
// report delivered back to port 32768 must bring the client to idle (no more datagrams)
#include <cstdio>
#include <cstdlib>
#include <hls_stream.h>
#include <ap_axi_sdata.h>
typedef ap_axiu<512, 0, 0, 0> nax_t; typedef ap_uint<176> meta_t;
#define FNS_DATA_WIDTH 64
#define FNS_ROCE_STACK_MAX_QPS 500
#include "iperf_udp/iperf_udp.hpp"
struct iperf_state { hls::stream<ipUdpMeta> f_rxm, f_txm; hls::stream<net_axis<512> > f_rxd, f_txd; hls::stream<bool> startS, stopS, doneS;
                     ap_uint<1> run; ap_uint<8> wc, gap; ap_uint<32> ip; };
typedef ap_uint<8> sig_t; struct clock_state { hls::stream<bool> st, sp; };
void iperf_step(hls::stream<meta_t> &, hls::stream<nax_t> &, hls::stream<meta_t> &, hls::stream<nax_t> &, hls::stream<ap_uint<64> > &,
                hls::stream<sig_t> &, hls::stream<sig_t> &, iperf_state &);
void iperf_clock_step(hls::stream<sig_t> &, hls::stream<sig_t> &, clock_state &);
int main()
{
  hls::stream<meta_t> rxm, txm; hls::stream<nax_t> rxd, txd; hls::stream<ap_uint<64> > cfg; static iperf_state s;
  hls::stream<sig_t> start_s, stop_s; static clock_state cs;
  // STALL=1: the TX side drains only every 7th cycle (heavy back-pressure); the experiment must still end on time,
  // because the timer is its own kernel now (v1 stretched it by the stall factor)
  const int stall = getenv("STALL") ? 7 : 1;
  s.run = 0; s.wc = 22; s.gap = 0; s.ip = 0;
  const unsigned ip = 192 | 168u << 8 | 77u << 16 | 1u << 24, WC = 3;
  const unsigned want_ip = getenv("MUTATE") ? ip ^ 1 : ip;   // negative control: the expectation is wrong
  ap_uint<64> c = 0; c[0] = 1; c.range(15, 8) = WC; c.range(23, 16) = 0; c.range(63, 32) = ip; cfg.write(c);
  int dgrams = 0, bad = 0, words = 0; long last_seq = -1; bool saw_fin = false; int after_fin = 0, stop_seen = -1;
  for (int cyc = 0; cyc < 20000; cyc++) {
    if (cyc == 50) { ap_uint<64> z = c; z[0] = 0; cfg.write(z); }       // drop runExperiment once started (level-sensitive)
    iperf_step(rxm, rxd, txm, txd, cfg, start_s, stop_s, s);
    iperf_clock_step(start_s, stop_s, cs);
    if (stop_seen < 0 && !s.stopS.empty()) stop_seen = cyc;
    if (cyc % stall) continue;
    while (!txm.empty()) {
      meta_t m = txm.read(); dgrams++;
      if ((unsigned)m.range(127, 96) != want_ip || m.range(143, 128) != 5001 || m.range(159, 144) != 32768 || m.range(175, 160) != WC * 64) {
        bad++; if (bad < 5) printf("bad meta: ip %08x tport %u mport %u len %u\n", (unsigned)m.range(127, 96), (unsigned)m.range(143, 128), (unsigned)m.range(159, 144), (unsigned)m.range(175, 160)); }
      if (saw_fin) after_fin++;
    }
    while (!txd.empty()) {
      nax_t d = txd.read(); words++;
      if (words % WC == 1) {                                             // first word of a datagram: iperf header
        int seq = ((int)d.data.range(7, 0) << 24) | ((int)d.data.range(15, 8) << 16) | ((int)d.data.range(23, 16) << 8) | (int)d.data.range(31, 24);
        if (seq < 0) { if (-seq - 1 != last_seq && -seq != last_seq + 1) { /* fns: final id = -(seq) */ } saw_fin = true;
                       // the iperf2 server answers the final datagram with a report to the client's port
                       meta_t r = 0; r.range(127, 96) = ip; r.range(143, 128) = 5001; r.range(159, 144) = 32768; r.range(175, 160) = 8 + 128; rxm.write(r);
                       nax_t rd; rd.data = 0; rd.keep = (ap_uint<64>(1) << 0) - 1; rd.keep = ap_uint<64>(-1) >> 0; rd.last = 1; rd.keep = (1ull << 63) * 0 + 0xffffffffffffffffull >> 0; rxd.write(rd); }
        else { if (seq != last_seq + 1) { bad++; if (bad < 5) printf("seq %d after %ld\n", seq, last_seq); } last_seq = seq; }
      }
      if ((words % WC == 0) != (bool)d.last) { bad++; if (bad < 5) printf("last flag wrong at word %d\n", words); }
    }
  }
  printf("timer stop reached the client at cycle %d (csim END_TIME_5s = 1000 + start latency)\n", stop_seen);
  printf("iperf csim: %d datagrams (%d words), last seq %ld, final (negative) datagram %s, %d datagrams after it, %d problems\n",
         dgrams, words, last_seq, saw_fin ? "seen" : "MISSING", after_fin, bad);
  const bool ok = dgrams > 10 && words == dgrams * (int)WC && saw_fin && after_fin <= 1 && bad == 0 && stop_seen > 1000 && stop_seen < 1100;
  printf("iperf_fns csim: %s\n", ok ? "PASS" : "FAIL");
  return !ok;
}
