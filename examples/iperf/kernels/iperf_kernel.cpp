// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// iperf -- an iperf2 UDP server in the FPGA (stock `iperf -c <ip> -u` on the host is the client). Three kernels:
//
//   fld_rx --rxs--> iperf_srv --txs--> fld_tx        iperf_srv <--trq/ttk--> iperf_tmr (cycle counter)
//                     ^ scfg / srq, scnt v
//                     +----- iperf_sctl (host-started, one command per run)
//
// iperf_tmr  free-running: a 64-bit cycle counter; answers iperf_srv's timestamp requests without ever pausing.
// iperf_srv  free-running: for each frame from fld_rx, takes the arrival time right after the first beat; checks
//            Ethernet / IPv4 (no options) / UDP to the configured port (default 5001) and reads iperf2's datagram
//            header (iperf 2.2.1 include/payloads.h, struct UDP_datagram: id, tv_sec, tv_usec, id2, big-endian; the
//            sequence number is the signed 64-bit id2:id, negative on the client's final datagrams). Data datagrams
//            are counted, not echoed, with iperf2's own server accounting (src/Reporter.c): a sequence number below
//            the next expected one is out of order, a jump counts the skipped ones as lost, "datagrams" is the highest
//            sequence number seen, bytes are UDP payload bytes, jitter is RFC 1889 (J += (|D| - J) / 16 on the transit
//            time arrival - tv_sec:tv_usec). On each final datagram iperf_srv answers in place with iperf2's server
//            report (src/Reports.c write_UDP_AckFIN: a zeroed UDP_datagram + struct server_hdr, 128 bytes, flags
//            HEADER_VERSION1 | HEADER_SEQNO64B | SERVER_HEADER_EXTEND, see report_words), so the client's
//            `iperf -c` prints the FPGA's numbers as its "Server Report". The test's state stays until the next data
//            datagram after a final one starts a new test, so repeated finals (the client retries every 250 ms until a
//            report arrives) get the same report, and sequential tests need no reload. Anything else is dropped and
//            counted.
// iperf_sctl host-started (ap_ctrl_hs), one command per run (op, a, b) -> out[]:
//            1 SET   iperf_srv field a = b (F_PORT: UDP port; F_CPU: kernel clock cycles per microsecond)
//            2 COUNT out[0..NCNT-1] = iperf_srv's counters (C_*)
// HLS ORDER: Vitis HLS may schedule the read of a reply stream before the write of its request (another port): every
// request / reply pair has ap_wait() between them, and iperf_sctl writes all requests before polling for replies.
#include "etc/autopilot_ssdm_op.h"                                 // ap_wait()
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;    // FLD-lite stream: user[15:0] = frame length
typedef ap_axiu<16, 0, 0, 0> req_t;
typedef ap_axiu<64, 0, 0, 0> tick_t;
typedef ap_axiu<64, 0, 0, 0> scmd_t;     // to iperf_srv: data[7:0] field, data[63:32] value
typedef ap_axiu<32, 0, 0, 0> cnt_t;

enum { OP_SET = 1, OP_COUNT = 2 };
enum { F_PORT = 0, F_CPU, F_N };
// counters: cumulative (frames in, data datagrams, final datagrams, reports sent, frames dropped, tests started), then
// the current / last test (datagrams received, payload bytes lo / hi, highest sequence number lo, lost (iperf2's
// error count: lost - out of order), out of order, duplicates (= the previous number), first-to-last arrival lo / hi
// (cycles), jitter (cycles, Q4))
enum { C_FRAMES = 0, C_DATA, C_FIN, C_REPORTS, C_DROP, C_TESTS, T_RX, T_BYTES_LO, T_BYTES_HI, T_MAXID, T_LOST, T_OOO,
       T_DUP, T_DUR_LO, T_DUR_HI, T_JITQ4, NCNT };
static const unsigned REPORT_LEN = 14 + 20 + 8 + 16 + 112;         // the report frame: 170 bytes, 3 beats

static inline unsigned g8(const beat_t &d, int i) { return d.range(8 * i + 7, 8 * i); }
static inline void p8(beat_t &d, int i, unsigned v) { d.range(8 * i + 7, 8 * i) = v & 0xff; }
static inline uint32_t gbe32(const beat_t &d, int i) { return (g8(d, i) << 24) | (g8(d, i + 1) << 16) | (g8(d, i + 2) << 8) | g8(d, i + 3); }

// ------------------------------------------------------------------------------------------------ iperf_tmr
extern "C" void iperf_tmr(hls::stream<req_t> &rq, hls::stream<tick_t> &tk)
{
#pragma HLS INTERFACE axis port=rq
#pragma HLS INTERFACE axis port=tk
#pragma HLS INTERFACE ap_ctrl_none port=return
  ap_uint<64> t = 0;
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    t++;
    req_t r;
    if (rq.read_nb(r)) { tick_t w; w.data = t; w.keep = -1; w.strb = -1; w.last = 1; tk.write_nb(w); }
  }
}

// ------------------------------------------------------------------------------------------------ iperf_srv
struct test_t {
  bool active, done;                     // a test is running / has seen its final datagram
  ap_uint<64> first, last;               // arrival of the first / last data datagram (cycles)
  ap_int<64> maxid;                      // iperf2's PacketID: the highest sequence number seen
  ap_uint<64> bytes;
  uint32_t rx, lost, ooo, dup;
  ap_int<64> ptransit; bool havet;       // previous transit time (cycles), for the jitter
  ap_uint<64> jq4;                       // jitter (cycles) x 16
};
struct srv_state { uint32_t f[F_N]; uint32_t c[NCNT]; test_t t; };

static void test_reset(test_t &t)
{
  t.active = false; t.done = false; t.first = 0; t.last = 0; t.maxid = 0; t.bytes = 0;
  t.rx = 0; t.lost = 0; t.ooo = 0; t.dup = 0; t.ptransit = 0; t.havet = false; t.jq4 = 0;
}

void srv_init(srv_state &s)
{
  for (int i = 0; i < NCNT; i++) s.c[i] = 0;
  s.f[F_PORT] = 5001; s.f[F_CPU] = 300;
  test_reset(s.t);
}

#ifndef __SYNTHESIS__
uint64_t iperf_tb_now;                   // C simulation: the testbench's clock (cycles)
#endif
static inline ap_uint<64> stamp(hls::stream<req_t> &trq, hls::stream<tick_t> &ttk, unsigned dep)
{
#ifndef __SYNTHESIS__
  (void)trq; (void)ttk; (void)dep;
  return iperf_tb_now;
#else
  req_t q; q.data = dep & 0xffff; q.keep = -1; q.strb = -1; q.last = 1;   // dep: data just read, so the request cannot move ahead of it
  trq.write(q);
  ap_wait();
  return ttk.read().data;
#endif
}

// the counters of the current test, as the host reads them
static void test_counters(srv_state &s)
{
  const test_t &t = s.t;
  const ap_uint<64> dur = t.last - t.first;
  s.c[T_RX] = t.rx; s.c[T_BYTES_LO] = t.bytes.range(31, 0); s.c[T_BYTES_HI] = t.bytes.range(63, 32);
  s.c[T_MAXID] = (uint32_t)t.maxid.range(31, 0);
  s.c[T_LOST] = t.lost > t.ooo ? t.lost - t.ooo : 0; s.c[T_OOO] = t.ooo; s.c[T_DUP] = t.dup;
  s.c[T_DUR_LO] = dur.range(31, 0); s.c[T_DUR_HI] = dur.range(63, 32); s.c[T_JITQ4] = (uint32_t)t.jq4.range(31, 0);
}

// the 112-byte server_hdr, big-endian, as 28 words: server_hdr_v1 (0-9), server_hdr_extension (10-24),
// server_hdr_extension2 (25-27). SERVER_HEADER_EXTEND is set because iperf2 2.2.1's client prints the relayed
// jitter only with a non-zero cntIPG (datagrams per second, which it prints as pps); the transit-time words are
// unknown here (the client's clock is not ours), so minTransit is set beyond the client's "unrealistic" limit (60 s)
// and it prints "-/-/-/- ms" for them instead of made-up latencies
static const uint32_t SERVER_HEADER_EXTEND = 0x40000000u;
static void report_words(const test_t &t, uint32_t cpu, uint32_t w[28])
{
#pragma HLS INLINE off
  const ap_uint<64> dur = t.last - t.first;
  const ap_uint<64> f = ap_uint<64>(cpu) * 1000000;
  const ap_uint<64> sec = f ? ap_uint<64>(dur / f) : ap_uint<64>(0);
  const ap_uint<64> usec = (f && cpu) ? ap_uint<64>((dur - sec * f) / cpu) : ap_uint<64>(0);
  const ap_uint<64> jus = cpu ? ap_uint<64>((t.jq4 >> 4) / cpu) : ap_uint<64>(0);
  const ap_uint<64> err = t.lost > t.ooo ? t.lost - t.ooo : 0;
  const ap_uint<64> dg = t.maxid < 0 ? ap_uint<64>(0) : ap_uint<64>(t.maxid);
  for (int i = 0; i < 28; i++) w[i] = 0;
  const ap_uint<64> pps = dur ? ap_uint<64>(ap_uint<64>(t.rx) * f / dur) : ap_uint<64>(0);
  w[0] = 0x80000000u | 0x08000000u | SERVER_HEADER_EXTEND;   // HEADER_VERSION1 | HEADER_SEQNO64B | SERVER_HEADER_EXTEND
  w[1] = t.bytes.range(63, 32); w[2] = t.bytes.range(31, 0);
  w[3] = sec.range(31, 0); w[4] = usec.range(31, 0);
  w[5] = err.range(31, 0); w[6] = t.ooo; w[7] = dg.range(31, 0);
  w[8] = (jus / 1000000).range(31, 0); w[9] = (jus % 1000000).range(31, 0);
  w[10] = 0x7fffffff;                    // minTransit1: "unrealistic" -> the client suppresses the latency columns
  w[23] = pps.range(31, 0); w[24] = 1;   // cntIPG, IPGsum (1 s): the client prints cntIPG / IPGsum as pps
  w[25] = err.range(63, 32); w[26] = 0; w[27] = dg.range(63, 32);  // error_cnt2, outorder_cnt2, datagrams2
}

// the report frame (3 beats) from the final datagram's first beat b0: addresses and ports swapped, lengths set,
// checksums zero (fld_tx has the NIC compute them)
static void send_report(beat_t b0, ap_uint<32> user, const uint32_t w[28], hls::stream<pkt_t> &out)
{
#pragma HLS INLINE off
  const beat_t s0 = b0;
  for (int i = 0; i < 6; i++) { p8(b0, i, g8(s0, 6 + i)); p8(b0, 6 + i, g8(s0, i)); }            // MACs
  for (int i = 0; i < 4; i++) { p8(b0, 26 + i, g8(s0, 30 + i)); p8(b0, 30 + i, g8(s0, 26 + i)); }  // IPs
  for (int i = 0; i < 2; i++) { p8(b0, 34 + i, g8(s0, 36 + i)); p8(b0, 36 + i, g8(s0, 34 + i)); }  // ports
  p8(b0, 16, (REPORT_LEN - 14) >> 8); p8(b0, 17, (REPORT_LEN - 14) & 0xff);                       // IPv4 total length
  p8(b0, 24, 0); p8(b0, 25, 0);
  p8(b0, 38, (REPORT_LEN - 34) >> 8); p8(b0, 39, (REPORT_LEN - 34) & 0xff);                       // UDP length
  p8(b0, 40, 0); p8(b0, 41, 0);
  uint8_t r[16 + 112];                    // payload: UDP_datagram (zero, as iperf2's server sends it) + server_hdr
  for (int i = 0; i < 16; i++) r[i] = 0;
  for (int i = 0; i < 28; i++) { r[16 + 4 * i] = w[i] >> 24; r[17 + 4 * i] = w[i] >> 16; r[18 + 4 * i] = w[i] >> 8; r[19 + 4 * i] = w[i]; }
  beat_t b1 = 0, b2 = 0;
  for (int i = 0; i < 22; i++) p8(b0, 42 + i, r[i]);                                            // frame 42..63
  for (int i = 0; i < 64; i++) p8(b1, i, r[22 + i]);                                            // 64..127
  for (int i = 0; i < 42; i++) p8(b2, i, r[86 + i]);                                            // 128..169
  pkt_t o; o.strb = -1; o.user = user; o.user.range(15, 0) = REPORT_LEN;
  o.data = b0; o.keep = -1; o.last = 0; out.write(o);
  o.data = b1; out.write(o);
  o.data = b2; o.keep = (ap_uint<64>(1) << 42) - 1; o.last = 1; out.write(o);
}

// one frame whose first beat p0 has been read
void srv_frame(pkt_t p0, hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq,
               hls::stream<tick_t> &ttk, srv_state &s)
{
#pragma HLS INLINE off
  const beat_t b0 = p0.data;
  const ap_uint<64> now = stamp(trq, ttk, g8(b0, 42));
  s.c[C_FRAMES]++;
  bool last = p0.last;
drain:
  while (!last) {
#pragma HLS PIPELINE II=1
    pkt_t p = in.read();
    last = p.last;
  }
  const unsigned len = p0.user.range(15, 0);
  const unsigned port = s.f[F_PORT];
  const unsigned ulen = (g8(b0, 38) << 8) | g8(b0, 39);
  const bool hdr = len >= 42 + 16 && g8(b0, 12) == 0x08 && g8(b0, 13) == 0x00 && g8(b0, 14) == 0x45 &&
                   g8(b0, 23) == 17 && g8(b0, 36) == ((port >> 8) & 0xff) && g8(b0, 37) == (port & 0xff) &&
                   ulen >= 8 + 16 && ulen <= len - 34;
  if (!hdr) { s.c[C_DROP]++; return; }
  const uint32_t id = gbe32(b0, 42), sec = gbe32(b0, 46), usec = gbe32(b0, 50), id2 = gbe32(b0, 54);
  ap_int<64> seq; seq.range(31, 0) = id; seq.range(63, 32) = id2;
  test_t &t = s.t;
  if (seq < 0) {                          // final datagram: report (again, if the client retries)
    s.c[C_FIN]++;
    t.done = true;
    test_counters(s);
    uint32_t w[28];
    report_words(t, s.f[F_CPU], w);
    send_report(b0, p0.user, w, out);
    s.c[C_REPORTS]++;
    return;
  }
  if (!t.active || t.done) { test_reset(t); t.active = true; t.first = now; s.c[C_TESTS]++; }
  s.c[C_DATA]++;
  t.rx++;
  t.bytes += ulen - 8;
  t.last = now;
  if (seq != t.maxid + 1) {
    if (seq < t.maxid + 1) { t.ooo++; if (seq == t.maxid) t.dup++; }
    else t.lost += (uint32_t)(seq - t.maxid - 1);
  }
  if (seq > t.maxid) t.maxid = seq;
  // RFC 1889 jitter in cycles: transit = arrival - sent (the client's clock, in microseconds x cycles per microsecond)
  const ap_int<64> sent = (ap_int<64>(sec) * 1000000 + usec) * s.f[F_CPU];
  const ap_int<64> transit = ap_int<64>(now) - sent;
  if (t.havet) {
    ap_int<64> d = transit - t.ptransit; if (d < 0) d = -d;
    t.jq4 = t.jq4 + ap_uint<64>(d) - (t.jq4 >> 4);
  }
  t.ptransit = transit; t.havet = true;
  test_counters(s);
}

// one turn of the free-running loop: a field write, a counter request, or a frame
void srv_step(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq, hls::stream<tick_t> &ttk,
              hls::stream<scmd_t> &scfg, hls::stream<req_t> &srq, hls::stream<cnt_t> &scnt, srv_state &s)
{
#pragma HLS INLINE
  scmd_t c;
  if (scfg.read_nb(c)) { const unsigned k = c.data.range(7, 0); if (k < F_N) s.f[k] = c.data.range(63, 32); }
  req_t r;
  if (srq.read_nb(r)) {
  cn:
    for (int i = 0; i < NCNT; i++) {
      cnt_t w; w.data = s.c[i]; w.keep = -1; w.strb = -1; w.last = i == NCNT - 1; scnt.write(w);
    }
  }
  pkt_t p0;
  if (in.read_nb(p0)) srv_frame(p0, in, out, trq, ttk, s);
}

extern "C" void iperf_srv(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq,
                          hls::stream<tick_t> &ttk, hls::stream<scmd_t> &scfg, hls::stream<req_t> &srq,
                          hls::stream<cnt_t> &scnt)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE axis port=trq
#pragma HLS INTERFACE axis port=ttk
#pragma HLS INTERFACE axis port=scfg
#pragma HLS INTERFACE axis port=srq
#pragma HLS INTERFACE axis port=scnt
#pragma HLS INTERFACE ap_ctrl_none port=return
  srv_state s;
  srv_init(s);
lp:
  while (1) {
#pragma HLS PIPELINE off
    srv_step(in, out, trq, ttk, scfg, srq, scnt, s);
  }
}

#ifndef __SYNTHESIS__
// testbench hook: one turn of iperf_srv's loop with its own state (the kernel itself loops forever)
void iperf_srv_tb_turn(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<scmd_t> &scfg,
                       hls::stream<req_t> &srq, hls::stream<cnt_t> &scnt)
{
  static srv_state s; static bool init = false;
  static hls::stream<req_t> trq; static hls::stream<tick_t> ttk;
  if (!init) { srv_init(s); init = true; }
  srv_step(in, out, trq, ttk, scfg, srq, scnt, s);
}
#endif

// ------------------------------------------------------------------------------------------------ iperf_sctl
extern "C" void iperf_sctl(unsigned op, unsigned a, unsigned b, uint32_t *out, hls::stream<scmd_t> &scfg,
                           hls::stream<req_t> &srq, hls::stream<cnt_t> &scnt)
{
#pragma HLS INTERFACE m_axi port=out offset=slave bundle=gmem0 depth=16 max_widen_bitwidth=32 max_read_burst_length=2 max_write_burst_length=16 num_read_outstanding=1 num_write_outstanding=1
#pragma HLS INTERFACE axis port=scfg
#pragma HLS INTERFACE axis port=srq
#pragma HLS INTERFACE axis port=scnt
  // the request first (one write site per stream), then the replies in a polling loop (see HLS ORDER above)
  if (op == OP_SET) {
    scmd_t c; c.keep = -1; c.strb = -1; c.last = 1; c.data = 0; c.data.range(7, 0) = a; c.data.range(63, 32) = b;
    scfg.write(c);
  }
  if (op == OP_COUNT) { req_t q; q.data = 1; q.keep = -1; q.strb = -1; q.last = 1; srq.write(q); }
  ap_wait();
  if (op == OP_COUNT) {
    uint32_t c[NCNT];
    int i = 0;
  rd:
    while (i < NCNT) {
#pragma HLS PIPELINE off
      cnt_t w;
      if (scnt.read_nb(w)) c[i++] = w.data;
    }
  wr:
    for (int k = 0; k < NCNT; k++) out[k] = c[k];
  }
}
