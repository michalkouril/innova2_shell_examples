// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ntp -- an NTP server whose replies are made in the FPGA (see ../DESIGN.md). Three kernels:
//
//   fld_rx --rxs--> ntp_resp --txs--> fld_tx           ntp_resp <--trq/ttk--> ntp_tod (time of day)
//                      ^ rcfg / rrq, rcnt v                ^ tcfg / tsnap v
//                      +------------- ntp_ctl (host-started, one command per run) ---+
//
// ntp_tod  free-running: a 96-bit time-of-day accumulator in units of 2^-64 s whose top 64 bits are an NTP
//          timestamp (32-bit seconds since 1900, 32-bit fraction). Every cycle tod += rate (2^-64 s per cycle,
//          nominally 2^64 / f_clk); the host trims rate (frequency) and steps the phase. It answers timestamp
//          requests from ntp_resp and snapshot requests from ntp_ctl, never blocking (the counter must not pause).
// ntp_resp free-running: for each frame from fld_rx, takes T2 (receive) right after the first beat arrives; checks
//          Ethernet / IPv4 (no options) / UDP dport 123 / 48-byte NTP mode 3 version 3 or 4; replies in place (MACs,
//          IPs, ports swapped, checksums zeroed: fld_tx has the NIC recompute them) with mode 4, the host-set leap,
//          stratum, precision, root delay / dispersion, refid and reference timestamp, origin = the request's
//          transmit timestamp, receive = T2 - cal_rx, transmit = T3 + cal_tx (T3 taken just before the beat that
//          carries it, after which fld_tx only writes its WQE and rings the doorbell). Anything else is dropped and
//          counted. No PPS input in this version: the time base is disciplined by the host (ntp_servo.py).
// ntp_ctl  host-started (ap_ctrl_hs), one command per run (op, a, b) -> out[]:
//          1 RATE  rate = b:a (2^-64 s per cycle)        2 STEP  tod += sign-extended b:a (2^-32 s units)
//          3 SNAP  out[0..1] = tod (NTP, lo/hi)            4 SET   ntp_resp field a = b (see RF_*)
//          5 COUNT out[0..NCNT-1] = ntp_resp's counters, out[NCNT..] = ntp_tod's
// HLS ORDER: Vitis HLS treats different stream ports as independent and may schedule a read of a reply stream BEFORE
// the write of the request that produces it (hardware deadlock; C simulation cannot show it). Every request/reply pair
// below has ap_wait() between them; check <prj>/.autopilot/db/<kernel>.verbose.sched.rpt (the write's state must come
// before the read's).
#include "etc/autopilot_ssdm_op.h"                                 // ap_wait()
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;    // FLD-lite stream: user[15:0] = frame length
typedef ap_axiu<16, 0, 0, 0> req_t;
typedef ap_axiu<64, 0, 0, 0> tick_t;
typedef ap_axiu<128, 0, 0, 0> tcmd_t;    // to ntp_tod: data[7:0] op, data[127:64] value
typedef ap_axiu<64, 0, 0, 0> rcmd_t;     // to ntp_resp: data[7:0] field, data[63:32] value
typedef ap_axiu<32, 0, 0, 0> cnt_t;

enum { OP_RATE = 1, OP_STEP = 2, OP_SNAP = 3, OP_SET = 4, OP_COUNT = 5 };
// ntp_resp fields (OP_SET a): leap 0..3, stratum, precision (int8), root delay, root dispersion (NTP short 16.16),
// refid (4 bytes, first byte in bits 31:24), reference timestamp lo / hi, cal_rx / cal_tx (2^-32 s units)
enum { RF_LEAP = 0, RF_STRATUM, RF_PREC, RF_RDELAY, RF_RDISP, RF_REFID, RF_REF_LO, RF_REF_HI, RF_CAL_RX, RF_CAL_TX, RF_N };
// ntp_resp counters: frames in, replies, dropped (short / long), dropped (not IPv4/UDP 123), dropped (mode/version),
// last residence T3 - T2 (2^-32 s), largest residence
enum { C_FRAMES = 0, C_REPLIES, C_DROP_LEN, C_DROP_HDR, C_DROP_MODE, C_RES_LAST, C_RES_MAX, NCNT };
static const unsigned NTOD = 2;          // ntp_tod: timestamp writes that found the stream full, commands applied
static const uint64_t RATE_300 = 61489146912ull;   // 2^64 / 300 MHz

static inline unsigned g8(const beat_t &d, int i) { return d.range(8 * i + 7, 8 * i); }
static inline void p8(beat_t &d, int i, unsigned v) { d.range(8 * i + 7, 8 * i) = v & 0xff; }
static inline void pbe32(beat_t &d, int i, uint32_t v) { p8(d, i, v >> 24); p8(d, i + 1, v >> 16); p8(d, i + 2, v >> 8); p8(d, i + 3, v); }
static inline void pbe64(beat_t &d, int i, ap_uint<64> v) { pbe32(d, i, v.range(63, 32)); pbe32(d, i + 4, v.range(31, 0)); }

// ------------------------------------------------------------------------------------------------ ntp_tod
struct tod_state { ap_uint<96> acc; ap_uint<96> add; ap_uint<64> rate; uint32_t full, cmds; };
static inline void tod_init(tod_state &s) { s.acc = 0; s.rate = RATE_300; s.add = RATE_300; s.full = 0; s.cmds = 0; }

// one cycle (the kernel loops it; the testbench calls it)
void tod_step(tod_state &s, hls::stream<tcmd_t> &cfg, hls::stream<tick_t> &snap, hls::stream<req_t> &rq,
              hls::stream<tick_t> &tk)
{
#pragma HLS INLINE
  s.acc += s.add;                        // add is a register: one 96-bit add per cycle
  ap_uint<96> nadd = s.rate;
  tick_t t; t.data = s.acc.range(95, 32); t.keep = -1; t.strb = -1; t.last = 1;
  // one access per stream per cycle (two write_nb calls on snap, even in exclusive branches, cost II=2)
  bool miss = false;
  req_t r;
  if (rq.read_nb(r)) miss = !tk.write_nb(t);
  tcmd_t c;
  if (cfg.read_nb(c)) {
    const unsigned op = c.data.range(7, 0);
    const ap_uint<64> v = c.data.range(127, 64);
    s.cmds++;
    if (op == OP_RATE) { s.rate = v; nadd = v; }
    else if (op == OP_STEP) { ap_int<96> st = ap_int<64>(v); nadd = s.rate + (ap_uint<96>(st) << 32); }
    if (op == OP_SNAP || op == OP_COUNT) {
      tick_t w = t;
      if (op == OP_COUNT) { w.data.range(31, 0) = s.full; w.data.range(63, 32) = s.cmds; }
      if (!snap.write_nb(w)) miss = true;
    }
  }
  if (miss) s.full++;
  s.add = nadd;
}

extern "C" void ntp_tod(hls::stream<tcmd_t> &cfg, hls::stream<tick_t> &snap, hls::stream<req_t> &rq, hls::stream<tick_t> &tk)
{
#pragma HLS INTERFACE axis port=cfg
#pragma HLS INTERFACE axis port=snap
#pragma HLS INTERFACE axis port=rq
#pragma HLS INTERFACE axis port=tk
#pragma HLS INTERFACE ap_ctrl_none port=return
  tod_state s;
  tod_init(s);
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    tod_step(s, cfg, snap, rq, tk);
  }
}

// ------------------------------------------------------------------------------------------------ ntp_resp
struct resp_state { uint32_t f[RF_N]; uint32_t c[NCNT]; };
void resp_init(resp_state &s)
{
  for (int i = 0; i < RF_N; i++) s.f[i] = 0;
  for (int i = 0; i < NCNT; i++) s.c[i] = 0;
  s.f[RF_LEAP] = 3; s.f[RF_STRATUM] = 16; s.f[RF_PREC] = (uint32_t)-28;     // unsynchronised until the host says so
}

static inline ap_uint<64> stamp(hls::stream<req_t> &trq, hls::stream<tick_t> &ttk, unsigned dep)
{
  req_t q; q.data = dep & 0xffff; q.keep = -1; q.strb = -1; q.last = 1;   // dep: data just read, so the request cannot move ahead of it
  trq.write(q);
  ap_wait();
  return ttk.read().data;
}

// one frame whose first beat p0 has been read
void ntp_frame(pkt_t p0, hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq,
               hls::stream<tick_t> &ttk, resp_state &s)
{
#pragma HLS INLINE off
  beat_t b0 = p0.data;
  const ap_uint<64> t2 = stamp(trq, ttk, g8(b0, 42));
  s.c[C_FRAMES]++;
  const unsigned len = p0.user.range(15, 0);
  pkt_t p1;
  bool last = p0.last, two = false;
  if (!last) { p1 = in.read(); last = p1.last; two = true; }
  bool extra = false;
drain:
  while (!last) {
#pragma HLS PIPELINE II=1
    pkt_t p = in.read();
    last = p.last; extra = true;
  }
  if (!two || extra || len != 90) { s.c[C_DROP_LEN]++; return; }
  beat_t b1 = p1.data;
  const bool hdr = g8(b0, 12) == 0x08 && g8(b0, 13) == 0x00 && g8(b0, 14) == 0x45 && g8(b0, 23) == 17 &&
                   g8(b0, 36) == 0 && g8(b0, 37) == 123 && g8(b0, 38) == 0 && g8(b0, 39) == 56;
  if (!hdr) { s.c[C_DROP_HDR]++; return; }
  const unsigned lvm = g8(b0, 42), vn = (lvm >> 3) & 7, mode = lvm & 7;
  if (mode != 3 || (vn != 3 && vn != 4)) { s.c[C_DROP_MODE]++; return; }
  // NTP byte i is frame byte 42 + i: bytes 0..21 in beat 0 (42..63), 22..47 in beat 1 (0..25)
  ap_uint<64> org;                        // the request's transmit timestamp (NTP 40..47 = beat 1 18..25), as bytes
  for (int i = 0; i < 8; i++) org.range(63 - 8 * i, 56 - 8 * i) = g8(b1, 18 + i);
  const beat_t s0 = b0;
  for (int i = 0; i < 6; i++) { p8(b0, i, g8(s0, 6 + i)); p8(b0, 6 + i, g8(s0, i)); }            // MACs
  for (int i = 0; i < 4; i++) { p8(b0, 26 + i, g8(s0, 30 + i)); p8(b0, 30 + i, g8(s0, 26 + i)); }  // IPs
  for (int i = 0; i < 2; i++) { p8(b0, 34 + i, g8(s0, 36 + i)); p8(b0, 36 + i, g8(s0, 34 + i)); }  // ports
  p8(b0, 24, 0); p8(b0, 25, 0); p8(b0, 40, 0); p8(b0, 41, 0);                                 // checksums -> NIC
  p8(b0, 42, (s.f[RF_LEAP] & 3) << 6 | vn << 3 | 4);
  p8(b0, 43, s.f[RF_STRATUM]);
  // 44: poll, copied
  p8(b0, 45, s.f[RF_PREC]);
  pbe32(b0, 46, s.f[RF_RDELAY]);
  pbe32(b0, 50, s.f[RF_RDISP]);
  pbe32(b0, 54, s.f[RF_REFID]);
  ap_uint<64> ref; ref.range(63, 32) = s.f[RF_REF_HI]; ref.range(31, 0) = s.f[RF_REF_LO];
  for (int i = 0; i < 6; i++) p8(b0, 58 + i, ref.range(63 - 8 * i, 56 - 8 * i));               // NTP 16..21
  p8(b1, 0, ref.range(15, 8)); p8(b1, 1, ref.range(7, 0));                                    // NTP 22..23
  for (int i = 0; i < 8; i++) p8(b1, 2 + i, org.range(63 - 8 * i, 56 - 8 * i));                // origin NTP 24..31
  pbe64(b1, 10, t2 - s.f[RF_CAL_RX]);                                                         // receive NTP 32..39
  p0.data = b0;
  out.write(p0);
  const ap_uint<64> t3 = stamp(trq, ttk, g8(b0, 42));
  pbe64(b1, 18, t3 + s.f[RF_CAL_TX]);                                                         // transmit NTP 40..47
  p1.data = b1;
  out.write(p1);
  const uint32_t res = (uint32_t)(t3 - t2);
  s.c[C_RES_LAST] = res;
  if (res > s.c[C_RES_MAX]) s.c[C_RES_MAX] = res;
  s.c[C_REPLIES]++;
}

// one turn of the free-running loop: a field write, a counter request, or a frame
void resp_step(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq, hls::stream<tick_t> &ttk,
               hls::stream<rcmd_t> &rcfg, hls::stream<req_t> &rrq, hls::stream<cnt_t> &rcnt, resp_state &s)
{
#pragma HLS INLINE
  rcmd_t c;
  if (rcfg.read_nb(c)) { const unsigned k = c.data.range(7, 0); if (k < RF_N) s.f[k] = c.data.range(63, 32); }
  req_t r;
  if (rrq.read_nb(r)) {
  cn:
    for (int i = 0; i < NCNT; i++) {
      cnt_t w; w.data = s.c[i]; w.keep = -1; w.strb = -1; w.last = i == NCNT - 1; rcnt.write(w);
    }
  }
  pkt_t p0;
  if (in.read_nb(p0)) ntp_frame(p0, in, out, trq, ttk, s);
}

extern "C" void ntp_resp(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<req_t> &trq, hls::stream<tick_t> &ttk,
                         hls::stream<rcmd_t> &rcfg, hls::stream<req_t> &rrq, hls::stream<cnt_t> &rcnt)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE axis port=trq
#pragma HLS INTERFACE axis port=ttk
#pragma HLS INTERFACE axis port=rcfg
#pragma HLS INTERFACE axis port=rrq
#pragma HLS INTERFACE axis port=rcnt
#pragma HLS INTERFACE ap_ctrl_none port=return
  resp_state s;
  resp_init(s);
lp:
  while (1) {
#pragma HLS PIPELINE off
    resp_step(in, out, trq, ttk, rcfg, rrq, rcnt, s);
  }
}

// ------------------------------------------------------------------------------------------------ ntp_ctl
extern "C" void ntp_ctl(unsigned op, unsigned a, unsigned b, uint32_t *out, hls::stream<tcmd_t> &tcfg,
                        hls::stream<tick_t> &tsnap, hls::stream<rcmd_t> &rcfg, hls::stream<req_t> &rrq,
                        hls::stream<cnt_t> &rcnt)
{
#pragma HLS INTERFACE m_axi port=out offset=slave bundle=gmem0 depth=16 max_widen_bitwidth=32 max_read_burst_length=2 max_write_burst_length=16 num_read_outstanding=1 num_write_outstanding=1
#pragma HLS INTERFACE axis port=tcfg
#pragma HLS INTERFACE axis port=tsnap
#pragma HLS INTERFACE axis port=rcfg
#pragma HLS INTERFACE axis port=rrq
#pragma HLS INTERFACE axis port=rcnt
  // all requests first (one write site per stream), then the replies, each read in a polling loop: a loop is its own
  // scheduling region, so no reply read can be scheduled into the state of (or before) its request -- with plain
  // blocking reads HLS put the snapshot read in the same state as the request write (see HLS ORDER above)
  const bool tod = op == OP_RATE || op == OP_STEP || op == OP_SNAP || op == OP_COUNT;
  tcmd_t t; t.keep = -1; t.strb = -1; t.last = 1; t.data = 0;
  t.data.range(7, 0) = op; t.data.range(95, 64) = a; t.data.range(127, 96) = b;
  if (tod) tcfg.write(t);
  if (op == OP_SET) {
    rcmd_t c; c.keep = -1; c.strb = -1; c.last = 1; c.data = 0; c.data.range(7, 0) = a; c.data.range(63, 32) = b;
    rcfg.write(c);
  }
  if (op == OP_COUNT) { req_t q; q.data = 1; q.keep = -1; q.strb = -1; q.last = 1; rrq.write(q); }
  ap_wait();
  if (op == OP_COUNT) {
    uint32_t c[NCNT];
    int i = 0;
  rd:
    while (i < NCNT) {
#pragma HLS PIPELINE off
      cnt_t w;
      if (rcnt.read_nb(w)) c[i++] = w.data;
    }
  wr:
    for (int k = 0; k < NCNT; k++) out[k] = c[k];
  }
  if (op == OP_SNAP || op == OP_COUNT) {
    tick_t w;
    bool got = false;
  sn:
    while (!got) {
#pragma HLS PIPELINE off
      got = tsnap.read_nb(w);
    }
    const unsigned o = op == OP_SNAP ? 0 : NCNT;
    out[o] = w.data.range(31, 0); out[o + 1] = w.data.range(63, 32);
  }
}
