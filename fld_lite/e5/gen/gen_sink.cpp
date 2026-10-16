// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5 FPGA traffic generator (2026-09-30): drives the ConnectX's OTHER port (port 1) from the FPGA, so the device under
// test (FLD-lite + an AFU on port 0) sees traffic at line rate with no host CPU or host PCIe link in the path:
//
//   gen_tx --WQEs/doorbells (PEER window 1)--> port 1 TX --cable--> port 0 --> fld_rx -> AFU -> fld_tx --> port 0 TX
//   --cable--> port 1 RX --> RQ/CQ in FPGA memory --> sink_rx
//
// gen_tx: the host prebuilds K frames (K a power of two <= 256) in FPGA DDR and K one-WQEBB WQE templates (ctrl, eth
//   segment with the 18-byte inline header, pointer data segment). gen_tx loads the templates once, then posts frame i
//   with template i % K: patches the WQE index, SQN and CQ_UPDATE (every sig_every-th), writes BATCH WQEs back to back,
//   one DBR update, one acknowledgement wait, one 8-byte doorbell through PEER, then waits `gap` cycles (pacing).
//   flags bit 0 (BlueFlame): a batch of ONE WQE is written whole into the UAR's BlueFlame buffer (alternating the two
//   halves, bf_size apart) instead of the 8-byte doorbell, so the NIC takes the WQE from the doorbell write and never
//   reads the slot -- the measured SQ error is the NIC executing a slot with the PREVIOUS lap's contents.
//   flags bit 1: no zero-on-reap (A/B).
//   It reaps its own TX CQ only when the SQ runs short of space.
// sink_rx: polls the RX CQ 8 CQEs per read, counts frames/bytes/error CQEs, keeps the first and last CQE timestamps
//   (ConnectX clock) for the rate, and for every `sample`-th frame compares the frame byte-for-byte (length and data)
//   with the host-precomputed expected reply for its template k, read from frame byte id_off (echo frames carry k at
//   byte 14; ZUC replies in the low byte of the echoed message ID, byte 75). Mismatches therefore mean corrupted data
//   only; loss is frames posted by gen_tx minus frames counted here (and the port's rx_out_of_buffer counter).
#include <ap_int.h>
#include "etc/autopilot_ssdm_op.h"                                 // ap_wait()
#include <hls_burst_maxi.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
static inline void g_put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned g_get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline void g_put_be32(beat_t &b, unsigned k, unsigned v) { g_put8(b, k, v >> 24); g_put8(b, k + 1, v >> 16); g_put8(b, k + 2, v >> 8); g_put8(b, k + 3, v); }

// res: [0] frames posted, [1] batches (doorbells), [2] TX CQEs, [3] TX error CQEs, [4] first error word, [5] credit polls,
//      [6] loop iterations
// m_axi latency=1 everywhere: with the default (64) HLS schedules every request/response as a FIXED ~70-cycle
// operation and pads to it even when the data is back in ~49 cycles; latency=1 waits on the handshake instead.
// Measured (e51LA): FPGA echo 2.26 -> 1.95 us at 64 B, 3.85 -> 3.35 us at 1024 B.
extern "C" void gen_tx(hls::burst_maxi<beat_t> mem, hls::burst_maxi<beat_t> bfw, uint32_t *res,
                       unsigned tmpl_off, unsigned log_k, unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned sqn,
                       unsigned tcq_off, unsigned log_tcq, unsigned tcq_dbr_off, unsigned db_off,
                       unsigned n, unsigned batch, unsigned gap, unsigned sig_every, unsigned stop_off,
                       unsigned flags, unsigned bf_size)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=262144 max_read_burst_length=64 max_write_burst_length=2 num_write_outstanding=64 latency=1
#pragma HLS INTERFACE m_axi port=bfw offset=slave bundle=gmem1 depth=65536 max_write_burst_length=2 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem2 depth=16 latency=1
  const unsigned K = 1u << log_k, kmask = K - 1, sq_size = 1u << log_sq, sq_mask = sq_size - 1, tcq_mask = (1u << log_tcq) - 1;
  const unsigned bmax = batch == 0 ? 1 : batch > 48 ? 48 : batch;
  beat_t tmpl[256];
  mem.read_request(tmpl_off / 64, K);
ld:
  for (unsigned k = 0; k < K; k++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=256
    tmpl[k & 255] = mem.read();
  }
  unsigned sent = 0, pi = 0, twq = 0, tci = 0, batches = 0, cqes = 0, errs = 0, err0 = 0, cpolls = 0, iters = 0, sigc = 0, spin = 0xACE1u;
  bool db_out = false, stop = false;
  const bool use_bf = flags & 1, zero = !(flags & 2);
  unsigned bfsel = 0;
main:
  while (sent < n && !stop) {
#pragma HLS PIPELINE off
    iters++;
    // SQ space; reap the TX CQ only when short of it
    while (((pi - twq) & 0xffff) + bmax > sq_size && !stop) {
#pragma HLS PIPELINE off
      cpolls++;
      mem.read_request(tcq_off / 64 + (tci & tcq_mask), 1);
      const beat_t c = mem.read();
      const unsigned op_own = g_get8(c, 63), opcode = op_own >> 4;
      if (opcode != 0xF && (op_own & 1) == ((tci >> log_tcq) & 1)) {
        if (opcode != 0) { if (errs == 0) err0 = (op_own << 24) | (g_get8(c, 55) << 16) | (g_get8(c, 54) << 8); errs++; }
        // ZERO-ON-REAP: zero the completed slots [twq, wc] before they count as free -- the ConnectX was measured
        // executing a WQE with the previous lap's slot contents (WQE violation, vendor syndrome 0x68)
        const unsigned nt = (((g_get8(c, 60) << 8) | g_get8(c, 61)) + 1) & 0xffff;   // every WQE is one WQEBB
        const unsigned nz = zero ? (nt - twq) & 0xffff : 0;
      zc:
        for (unsigned base = 0; base < nz; base += 32) {
#pragma HLS PIPELINE off
          const unsigned m = nz - base < 32 ? nz - base : 32;
        zw:
          for (unsigned j = 0; j < m; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
            mem.write_request(sq_off / 64 + ((twq + base + j) & sq_mask), 1); mem.write(0);
          }
        za:
          for (unsigned j = 0; j < m; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
            mem.write_response();
          }
        }
        twq = nt;
        tci++; cqes++;
        beat_t d = 0; g_put_be32(d, 0, tci & 0xffffff);
        mem.write_request(tcq_dbr_off / 64, 1); mem.write(d); mem.write_response();
      } else if ((cpolls & 4095) == 0) {
        mem.read_request(stop_off / 64, 1);
        const beat_t s = mem.read();
        if (s.range(31, 0) != 0) stop = true;
      }
    }
    if (stop) break;
    const unsigned b = n - sent < bmax ? n - sent : bmax;
    beat_t last = 0;
  wr:
    for (unsigned j = 0; j < b; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=48
      beat_t w = tmpl[(sent + j) & kmask & 255];
      const unsigned idx = (pi + j) & 0xffff;
      const bool sig = sigc == sig_every - 1;
      sigc = sig ? 0 : sigc + 1;
      g_put8(w, 0, 0); g_put8(w, 1, idx >> 8); g_put8(w, 2, idx); g_put8(w, 3, 0x0a);
      g_put8(w, 4, sqn >> 16); g_put8(w, 5, sqn >> 8); g_put8(w, 6, sqn);
      g_put8(w, 11, sig ? 0x08 : 0x00);
      mem.write_request(sq_off / 64 + ((pi + j) & sq_mask), 1);
      mem.write(w);
      last = w;
    }
    pi = (pi + b) & 0xffff;
    beat_t d = 0; g_put_be32(d, 4, pi);
    mem.write_request(sq_dbr_off / 64, 1); mem.write(d);
  ack:
    for (unsigned j = 0; j < b + 1; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=49
      mem.write_response();
    }
    // ORDERING: the doorbell must not leave before the WQE/DBR write acknowledgements. They are on different m_axi
    // ports, so HLS sees no dependence and MAY hoist the doorbell above the ack loop: the e52z build did exactly that
    // (schedule: doorbell in the state before the ack call), and the NIC then executed a slot still holding the
    // PREVIOUS lap's WQE (error CQE opcode 13 / syndrome 0x02 / vendor 0x68). ap_wait() pins the order (verified in
    // the HLS schedule; a `protocol fixed` region is rejected because the ack loop has variable latency).
    ap_wait();
    if (use_bf && b == 1) {
      bfw.write_request((db_off + bfsel) / 64, 1);
      bfw.write(last);                                              // BlueFlame: the whole one-WQEBB WQE
      bfsel ^= bf_size;
    } else {
      bfw.write_request(db_off / 64, 1);
      bfw.write(last, 0xFF);                                        // 8-byte doorbell: the last WQE's ctrl
    }
    if (db_out) bfw.write_response();                               // the previous doorbell's acknowledgement
    db_out = true;
    sent += b; batches++;
  pace:
    for (unsigned g = 0; g < gap; g++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=1000
      // an LFSR step per cycle: a nonlinear recurrence HLS cannot collapse. (`spin++` was turned into `spin += gap`
      // and `x += (g == 0xffffffff)` into nothing -- both builds had NO pacing loop.) Consumed in res[7].
      spin = (spin >> 1) ^ ((0u - (spin & 1u)) & 0xB4BCD35Cu);
    }
  }
  if (db_out) bfw.write_response();
  res[0] = sent; res[1] = batches; res[2] = cqes; res[3] = errs; res[4] = err0; res[5] = cpolls; res[6] = iters; res[7] = spin;
}

// res64: [0] frames, [1] bytes, [2] error CQEs, [3] frames checked, [4] mismatches, [5] first CQE ts, [6] last CQE ts,
//        [7] polls, [8] first mismatch position, [9] length mismatches
extern "C" void sink_rx(hls::burst_maxi<beat_t> mem, hls::burst_maxi<beat_t> dbr, ap_uint<64> *res,
                        unsigned rcq_off, unsigned log_rcq, unsigned rcq_dbr_off, unsigned rq_dbr_off, unsigned log_rq,
                        unsigned data_off, unsigned stride, unsigned exp_off, unsigned exp_stride, unsigned elen_off,
                        unsigned log_k, unsigned log_sample, unsigned n, unsigned stop_off, unsigned id_off)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=262144 max_read_burst_length=32 num_read_outstanding=8 latency=1
#pragma HLS INTERFACE m_axi port=dbr offset=slave bundle=gmem1 depth=65536 max_write_burst_length=2 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem2 depth=16 latency=1
  const unsigned K = 1u << log_k, kmask = K - 1, rcq_n = 1u << log_rcq, rcq_mask = rcq_n - 1, rq_size = 1u << log_rq;
  const unsigned smask = (1u << log_sample) - 1;
  unsigned short elen[256];                                         // expected reply length per template
  mem.read_request(elen_off / 64, (K + 31) / 32);
lde:
  for (unsigned q = 0; q < (K + 31) / 32; q++) {
#pragma HLS PIPELINE off
#pragma HLS LOOP_TRIPCOUNT max=8
    const beat_t v = mem.read();
    for (int k = 0; k < 32; k++) elen[(q * 32 + k) & 255] = v.range(16 * k + 15, 16 * k);
  }
  unsigned rx = 0, pend = 0, idle = 0;
  ap_uint<64> bytes = 0, ts0 = 0, ts1 = 0;
  unsigned errs = 0, checked = 0, mism = 0, lmism = 0, polls = 0, first_bad = 0xffffffff;
  bool dbr_out = false, stop = false;
main:
  while (rx < n && !stop) {
#pragma HLS PIPELINE off
    polls++;
    // up to 8 CQEs per read, not across the ring's end
    const unsigned at = rx & rcq_mask, cnt = rcq_n - at < 8 ? rcq_n - at : 8;
    mem.read_request(rcq_off / 64 + at, cnt);
    beat_t c[8];
  rc:
    for (unsigned q = 0; q < 8; q++) {
#pragma HLS PIPELINE II=1
      c[q] = 0;
      if (q < cnt) c[q] = mem.read();
    }
    unsigned got = 0;
    bool more = true;
  cq:
    for (unsigned q = 0; q < 8; q++) {
#pragma HLS PIPELINE off
      if (!more || q >= cnt) break;
      const unsigned op_own = g_get8(c[q], 63), opcode = op_own >> 4, r = rx + q;
      if (opcode == 0xF || (op_own & 1) != ((r >> log_rcq) & 1)) { more = false; break; }
      const unsigned len = (g_get8(c[q], 46) << 8) | g_get8(c[q], 47);
      ap_uint<64> ts = 0; for (int k = 0; k < 8; k++) ts = (ts << 8) | g_get8(c[q], 48 + k);
      if (r == 0) ts0 = ts;
      ts1 = ts;
      if (opcode != 2) errs++;
      else {
        bytes += len;
        if ((r & smask) == 0) {                                     // sampled: compare with the expected reply
          checked++;
          const unsigned slot = ((g_get8(c[q], 60) << 8) | g_get8(c[q], 61)) & (rq_size - 1);
          const unsigned nb = (len + 63) / 64 > 256 ? 256 : (len + 63) / 64;
          mem.read_request((data_off + slot * stride) / 64, nb);
          beat_t fb[256];
        rf:
          for (unsigned z = 0; z < nb; z++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=24
            fb[z & 255] = mem.read();
          }
          const unsigned k = g_get8(fb[(id_off >> 6) & 255], id_off & 63) & kmask;   // the template this reply belongs to
          const unsigned want = elen[k & 255];
          bool ok = len == want;
          if (!ok) lmism++;
          if (ok) {
            mem.read_request((exp_off + k * exp_stride) / 64, nb);
          re:
            for (unsigned z = 0; z < nb; z++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=24
              const beat_t e = mem.read();
              // per-byte compare against the valid byte count (no wide shifter/subtractor: a 505-bit
              // (1 << 8*vb) - 1 failed 250 MHz by 1.76 ns)
              const unsigned vb = z == nb - 1 ? len - 64 * z : 64;
              const beat_t x = fb[z & 255] ^ e;
              bool diff = false;
              for (int y = 0; y < 64; y++) {
#pragma HLS UNROLL
                if ((unsigned)y < vb && x.range(8 * y + 7, 8 * y) != 0) diff = true;
              }
              if (diff) ok = false;
            }
          }
          if (!ok) { mism++; if (first_bad == 0xffffffff) first_bad = r; }
        }
      }
      got++;
    }
    if (got) {
      rx += got; pend += got; idle = 0;
      if (pend >= 64) {
        if (dbr_out) { dbr.write_response(); dbr.write_response(); }
        beat_t rcd = 0; g_put_be32(rcd, 0, rx & 0xffffff);
        beat_t rqd = 0; g_put_be32(rqd, 0, (rx + rq_size) & 0xffff);
        dbr.write_request(rcq_dbr_off / 64, 1); dbr.write(rcd);
        dbr.write_request(rq_dbr_off / 64, 1); dbr.write(rqd);
        dbr_out = true; pend = 0;
      }
    } else {
      if (pend) {
        if (dbr_out) { dbr.write_response(); dbr.write_response(); }
        beat_t rcd = 0; g_put_be32(rcd, 0, rx & 0xffffff);
        beat_t rqd = 0; g_put_be32(rqd, 0, (rx + rq_size) & 0xffff);
        dbr.write_request(rcq_dbr_off / 64, 1); dbr.write(rcd);
        dbr.write_request(rq_dbr_off / 64, 1); dbr.write(rqd);
        dbr_out = true; pend = 0;
      }
      if (++idle >= 4096) {
        idle = 0;
        mem.read_request(stop_off / 64, 1);
        const beat_t s = mem.read();
        if (s.range(31, 0) != 0) stop = true;
      }
    }
  }
  if (dbr_out) { dbr.write_response(); dbr.write_response(); }
  res[0] = rx; res[1] = bytes; res[2] = errs; res[3] = checked; res[4] = mism; res[5] = ts0; res[6] = ts1; res[7] = polls;
  res[8] = first_bad; res[9] = lmism;
}
