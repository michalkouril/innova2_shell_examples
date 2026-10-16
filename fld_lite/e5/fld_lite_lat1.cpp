// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.1 FLD-lite engines (2026-09-30). Four kernels joined by AXI streams (stream_connect), no DATAFLOW feedback:
//
//   fld_rx --rxs--> afu_loop --txs--> fld_tx <--cred-- fld_reap
//
// Rings (RQ, RX CQ, SQ, TX CQ, doorbell records) live in the URAM window of bank0 (HYBRID mode); RX frame slots and
// TX buffer slots in DDR. Doorbells/BlueFlame go to the ConnectX UAR through PEER.
// Latency rules from the E5.0 breakdown (work/logs/e50_breakdown.txt): hls::burst_maxi everywhere so that write
// acknowledgements are collected only where ordering needs them (WQE + buffer + DBR before the BlueFlame write);
// the BlueFlame/doorbell write and the bookkeeping DBR writes are never waited for on the critical path; the RX engine
// requests the expected frame slot right behind the CQE.
// RX ORDERING: the CQE (URAM) and the frame (DDR) are read on ONE port, CQE first. The HYBRID calib gate has no IDs
// and serves reads strictly in arrival order across URAM and DDR, and it writes the NIC's CQE to URAM only after the
// NIC's earlier frame write to DDR is acknowledged; so if the CQE read sees a new CQE, the frame read behind it sees
// the frame. On two ports the frame read could reach the gate first and return stale data under a valid CQE.
//
// E5.1b instrumentation (optional, stamp=1): fld_rx and fld_tx ask the free-running e5_ticker2 kernel for its cycle
// count (request carries a byte of the data just read, so a stamp cannot precede that read) and log per frame:
//   rx tlog[i] = {t0 CQE seen, t1 last beat streamed}; tx tlog[i] = {s0 first beat in, s1 WQE+DBR issued,
//   s2 acks back, s3 BlueFlame/doorbell issued}. Logs are written with deferred acknowledgements.
// Knobs: fld_rx spec=0 reads the frame only after the CQE is seen; fld_reap gap = idle cycles between polls.
// Stream beats (pkt_t): data = frame bytes in memory order (byte k at bits 8k+7..8k), keep/last as AXI-S,
// user[15:0] = frame length on every beat, user[31:16] = tag (0 for now).
#define AP_INT_MAX_W 2048                                                      // the 4-beat inline shift (ap_uint<2048>)
#include "etc/autopilot_ssdm_op.h"                                 // ap_wait()
#include <ap_int.h>
#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include <hls_burst_maxi.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;
typedef ap_axiu<32, 0, 0, 0> cred_t;
typedef ap_axiu<16, 0, 0, 0> req_t;
typedef ap_axiu<64, 0, 0, 0> tick_t;
static inline ap_uint<64> stamp_of(hls::stream<req_t> &req, hls::stream<tick_t> &tick, unsigned dep)
{
  req_t q; q.data = dep & 0xffff; q.keep = -1; q.strb = -1; q.last = 1; req.write(q);
  return tick.read().data;
}

static inline void put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline void put_be32(beat_t &b, unsigned k, unsigned v) { put8(b, k, v >> 24); put8(b, k + 1, v >> 16); put8(b, k + 2, v >> 8); put8(b, k + 3, v); }
static inline ap_uint<64> keep_of(unsigned bytes) { ap_uint<64> k = -1; if (bytes < 64) k = (ap_uint<64>(1) << bytes) - 1; return k; }

// ------------------------------------------------------------------------------------------------ RX engine
// E5.1c: every poll requests the CQE and, right behind it on the same port, the first `pre` beats (0..4) of the slot
// that CQE will normally point at; a frame longer than that fetches its remainder in ONE request. Each kernel memory
// round trip costs ~0.3 us on this platform (E5.1b stamps), so the number of serial requests is what matters.
// res: [0] CQEs, [1] frames streamed, [2] RX error CQEs, [3] slot mismatches (speculative read wasted), [4] polls
extern "C" void fld_rx(hls::burst_maxi<beat_t> mem, hls::burst_maxi<beat_t> dbr, uint32_t *res,
                       hls::stream<pkt_t> &rxs,
                       unsigned rcq_off, unsigned log_rcq, unsigned rcq_dbr_off, unsigned rq_dbr_off, unsigned log_rq,
                       unsigned data_off, unsigned stride, unsigned stop_off, unsigned batch,
                       unsigned pre, unsigned stamp, hls::burst_maxi<beat_t> tlog,
                       hls::stream<req_t> &sreq, hls::stream<tick_t> &stick)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=131072 max_read_burst_length=32 num_read_outstanding=4 max_write_burst_length=2 latency=1
#pragma HLS INTERFACE m_axi port=dbr offset=slave bundle=gmem1 depth=65536 max_write_burst_length=2 max_read_burst_length=2 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem2 depth=16 latency=1
#pragma HLS INTERFACE axis port=rxs
#pragma HLS INTERFACE m_axi port=tlog offset=slave bundle=gmem3 depth=65536 max_write_burst_length=2 latency=1
#pragma HLS INTERFACE axis port=sreq
#pragma HLS INTERFACE axis port=stick
  const unsigned rcq_mask = (1u << log_rcq) - 1, rq_size = 1u << log_rq, rq_mask = rq_size - 1;
  const unsigned npre = pre > 4 ? 4 : pre;
  unsigned rx = 0, frames = 0, errs = 0, mism = 0, polls = 0, pend = 0, idle = 0;
  bool dbr_out = false, stop = false;
  unsigned log_out = 0;

main:
  while (!stop) {
#pragma HLS PIPELINE off
    polls++;
    const unsigned exp = rx & rq_mask;
    // the CQE and, right behind it on the same port, the frame slot it will normally point at (see RX ORDERING)
    mem.read_request(rcq_off / 64 + (rx & rcq_mask), 1);
    if (npre) mem.read_request((data_off + exp * stride) / 64, npre);
    const beat_t c = mem.read();
    beat_t f[4];
  sp:
    for (unsigned q = 0; q < 4; q++) {
#pragma HLS PIPELINE II=1
      f[q] = 0;
      if (q < npre) f[q] = mem.read();
    }
    const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
    bool flush = false;
    if (opcode != 0xF && (op_own & 1) == ((rx >> log_rcq) & 1)) {
      ap_uint<64> t0 = 0;
      if (stamp) t0 = stamp_of(sreq, stick, op_own);
      const unsigned slot = ((get8(c, 60) << 8) | get8(c, 61)) & rq_mask;
      const unsigned len = (get8(c, 46) << 8) | get8(c, 47);
      if (opcode != 2 || len < 60 || len > stride) errs++;
      else {
        const unsigned nb = (len + 63) / 64;
        unsigned have = npre;
        if (slot != exp) { mism += npre != 0; have = 0; }
        if (have > nb) have = nb;
        if (nb > have) mem.read_request((data_off + slot * stride) / 64 + have, nb - have);   // the rest, one request
      out:
        for (unsigned q = 0; q < nb; q++) {                         // variable bound: a constant bound with an
#pragma HLS PIPELINE II=1                                           // if() runs every iteration (E5.1c lesson)
#pragma HLS LOOP_TRIPCOUNT max=130
          pkt_t p;
          p.data = q < have ? f[q & 3] : mem.read();
          p.keep = q == nb - 1 ? keep_of(len - 64 * q) : ap_uint<64>(-1);
          p.strb = p.keep;
          p.user = len & 0xffff;
          p.last = q == nb - 1;
          rxs.write(p);
        }
        frames++;
        if (stamp) {
          const ap_uint<64> t1 = stamp_of(sreq, stick, len);
          if (log_out) { tlog.write_response(); log_out--; }
          beat_t lr = 0; lr.range(63, 0) = t0; lr.range(127, 64) = t1; lr.range(159, 128) = rx;
          tlog.write_request(rx & 4095, 1); tlog.write(lr); log_out++;
        }
      }
      rx++; pend++; idle = 0;
      flush = pend >= batch;
    } else {
      flush = pend != 0;                                            // idle: publish consumption at once
      if (++idle >= 4096) {
        idle = 0;
        mem.read_request(stop_off / 64, 1);
        const beat_t s = mem.read();
        if (s.range(31, 0) != 0) stop = true;
      }
    }
    if (flush) {
      // CQ consumer index and RQ producer count; the previous pair's acknowledgements are collected only now
      if (dbr_out) { dbr.write_response(); dbr.write_response(); }
      beat_t rcd = 0; put_be32(rcd, 0, rx & 0xffffff);
      beat_t rqd = 0; put_be32(rqd, 0, (rx + rq_size) & 0xffff);
      dbr.write_request(rcq_dbr_off / 64, 1); dbr.write(rcd);
      dbr.write_request(rq_dbr_off / 64, 1); dbr.write(rqd);
      dbr_out = true; pend = 0;
    }
  }
  if (dbr_out) { dbr.write_response(); dbr.write_response(); }
  if (log_out) tlog.write_response();
  res[0] = rx; res[1] = frames; res[2] = errs; res[3] = mism; res[4] = polls;
}

// ------------------------------------------------------------------------------------------------ test AFU
// Free-running "AFU bypass": the echo's header rewrite (src MAC byte 6 = 0x02, byte 11 = 0xEC, ethertype 0x88B8).
extern "C" void afu_loop(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
  bool first = true;
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    pkt_t p = in.read();
    if (first) { beat_t d = p.data; put8(d, 6, 0x02); put8(d, 11, 0xEC); put8(d, 12, 0x88); put8(d, 13, 0xB8); p.data = d; }
    first = p.last;
    out.write(p);
  }
}

// ------------------------------------------------------------------------------------------------ TX engine
// E5.1c: frames waiting in the stream are BATCHED (up to maxb): their WQEs are written back to back, one DBR update,
// one acknowledgement wait, one doorbell (the last WQE's ctrl; the NIC fetches the batch from the URAM ring). A lone
// frame keeps the BlueFlame path. CQ_UPDATE every sig_every-th WQE by counter (a `%` here was a ~36-cycle divider).
// res: [0] frames sent, [1] WQEBBs, [2] inline frames, [3] buffered frames, [4] credit waits, [5] polls, [6] batches
extern "C" void fld_tx(hls::burst_maxi<beat_t> sq, hls::burst_maxi<beat_t> buf, hls::burst_maxi<beat_t> bfw, uint32_t *res,
                       hls::stream<pkt_t> &txs, hls::stream<cred_t> &cred,
                       unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned sqn, unsigned lkey,
                       unsigned buf_off, unsigned long long buf_va, unsigned log_nslots, unsigned slot_stride,
                       unsigned db_off, unsigned bf, unsigned bf_size, unsigned inline_max, unsigned sig_every,
                       unsigned stop_off, unsigned stamp, hls::burst_maxi<beat_t> tlog,
                       hls::stream<req_t> &sreq, hls::stream<tick_t> &stick, unsigned maxb)
{
#pragma HLS INTERFACE m_axi port=sq  offset=slave bundle=gmem0 depth=65536 max_write_burst_length=2 max_read_burst_length=2 num_write_outstanding=64 latency=1
#pragma HLS INTERFACE m_axi port=buf offset=slave bundle=gmem1 depth=131072 max_write_burst_length=32 num_write_outstanding=32 latency=1
#pragma HLS INTERFACE m_axi port=bfw offset=slave bundle=gmem2 depth=16384 max_write_burst_length=4 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem3 depth=16 latency=1
#pragma HLS INTERFACE axis port=txs
#pragma HLS INTERFACE axis port=cred
#pragma HLS INTERFACE m_axi port=tlog offset=slave bundle=gmem4 depth=65536 max_write_burst_length=2 latency=1
#pragma HLS INTERFACE axis port=sreq
#pragma HLS INTERFACE axis port=stick
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, nslots = 1u << log_nslots, slot_mask = nslots - 1;
  const unsigned bmax = maxb == 0 ? 1 : maxb > 16 ? 16 : maxb;
  unsigned pi = 0, twq = 0, sa = 0, sfree = 0, frames = 0, inl = 0, bufd = 0, waits = 0, polls = 0, idle = 0, batches = 0;
  unsigned bfsel = 0, sigc = 0;
  bool bf_out = false, stop = false;
  unsigned log_out = 0;
  unsigned short bbs_of[1024];                                     // WQEBBs of the WQE starting at index i
  unsigned short slots_after[1024];                                // buffer slots allocated up to that WQE

main:
  while (!stop) {
#pragma HLS PIPELINE off
    polls++;
    cred_t cr;
    if (cred.read_nb(cr)) {                                         // completions: free SQ space and buffer slots
      const unsigned wc = cr.data & 0xffff;
      twq = (wc + ((cr.data & 0x10000) ? 1u : (unsigned)bbs_of[wc & 1023])) & 0xffff;   // zero-on-reap: through wc only
      sfree = slots_after[wc & 1023];
    }
    pkt_t p;
    if (!txs.read_nb(p)) {
      if (++idle >= 4096) {
        idle = 0;
        sq.read_request(stop_off / 64, 1);
        const beat_t s = sq.read();
        if (s.range(31, 0) != 0) stop = true;
      }
      continue;
    }
    idle = 0;
    ap_uint<64> s0 = 0, s1 = 0, s2 = 0;
    if (stamp) s0 = stamp_of(sreq, stick, p.user);
    unsigned nw = 0, nack = 0, nbuf = 0, nbb = 0, lastbbs = 1;
    beat_t lw[4];
    bool more = true;
  batch:
    while (more) {
#pragma HLS PIPELINE off
      const unsigned len = p.user & 0xffff;
      const bool in = inline_max != 0 && len <= inline_max;
      const unsigned ds = in ? (len + 34 + 15) >> 4 : 4, bbs = (ds + 3) >> 2, nb = (len + 63) >> 6;
      // room in the SQ and (for a buffered frame) a free buffer slot; block on credits otherwise
    room:
      while (((pi - twq) & 0xffff) + bbs > sq_size || (!in && ((sa - sfree) & 0xffff) >= nslots)) {
#pragma HLS PIPELINE off
        cr = cred.read(); waits++;
        const unsigned wc = cr.data & 0xffff;
        twq = (wc + ((cr.data & 0x10000) ? 1u : (unsigned)bbs_of[wc & 1023])) & 0xffff;
        sfree = slots_after[wc & 1023];
      }
      beat_t f[4];
      f[0] = p.data; f[1] = 0; f[2] = 0; f[3] = 0;
      unsigned slot = 0;
      if (in) {
      rdi:
        for (unsigned q = 1; q < 4; q++) {
#pragma HLS PIPELINE II=1
          if (q < nb) f[q] = txs.read().data;
        }
        inl++;
      } else {
        slot = sa & slot_mask;
        buf.write_request((buf_off + slot * slot_stride) / 64, nb);
        buf.write(p.data);
      rdb:
        for (unsigned q = 1; q < nb; q++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=130
          buf.write(txs.read().data);
        }
        sa = (sa + 1) & 0xffff;
        bufd++; nbuf++; nbb += (nb + 31) >> 5;                      // AXI bursts of this copy (max_write_burst_length 32)
      }
      // WQE: ctrl + eth (18-byte inline L2 header) + data segment or inline data
      const bool sig = sigc == sig_every - 1;
      sigc = sig ? 0 : sigc + 1;
      beat_t w[4];
      w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
      put8(w[0], 1, pi >> 8); put8(w[0], 2, pi); put8(w[0], 3, 0x0a);
      put8(w[0], 4, sqn >> 16); put8(w[0], 5, sqn >> 8); put8(w[0], 6, sqn); put8(w[0], 7, ds);
      put8(w[0], 11, sig ? 0x08 : 0x00);
      put8(w[0], 29, 18);
      w[0].range(47 * 8 + 7, 30 * 8) = f[0].range(17 * 8 + 7, 0);   // inline L2 header, frame bytes 0..17
      if (in) {
        put_be32(w[0], 48, (len - 18) | 0x80000000u);
        const ap_uint<2048> F = (f[3], f[2], f[1], f[0]);
        const ap_uint<2048> S = (F >> (18 * 8)) << (52 * 8);
        ap_uint<2048> W = (w[3], w[2], w[1], w[0]);
        W.range(2047, 52 * 8) = S.range(2047, 52 * 8);
        w[0] = W.range(511, 0); w[1] = W.range(1023, 512); w[2] = W.range(1535, 1024); w[3] = W.range(2047, 1536);
      } else {
        const unsigned long long a = buf_va + (unsigned long long)slot * slot_stride + 18;
        put_be32(w[0], 48, len - 18); put_be32(w[0], 52, lkey);
        for (int q = 0; q < 8; q++) put8(w[0], 56 + q, a >> (8 * (7 - q)));
      }
      bbs_of[pi & 1023] = bbs;
      slots_after[pi & 1023] = sa;
    wr:
      for (unsigned q = 0; q < 4; q++) {
#pragma HLS PIPELINE II=1
        if (q < bbs) { sq.write_request(sq_off / 64 + ((pi + q) & sq_mask), 1); sq.write(w[q]); }
      }
      nack += bbs;
      pi = (pi + bbs) & 0xffff;
      lw[0] = w[0]; lw[1] = w[1]; lw[2] = w[2]; lw[3] = w[3]; lastbbs = bbs;
      frames++; nw++;
      // another frame already waiting: same batch. Bounded so the writes issued before the acknowledgement wait
      // never exceed the ports' outstanding-write capacity (64 sq, 32 buf bursts), which would deadlock the adapter.
      // The buffer bound counts BURSTS: a jumbo frame (up to 8320 B = 131 beats) is 5 of them, so leave room for 5.
      more = nw < bmax && nack + 5 <= 60 && nbb + 5 <= 30 && txs.read_nb(p);
    }
    beat_t d = 0; put_be32(d, 4, pi);
    sq.write_request(sq_dbr_off / 64, 1); sq.write(d);
    nack++;
    if (stamp) s1 = stamp_of(sreq, stick, pi);
    // ordering: the batch's WQEs, the DBR and the frame copies must have landed before the ConnectX is told
  ack:
    for (unsigned q = 0; q < nack; q++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=64
      sq.write_response();
    }
  ackb:
    for (unsigned q = 0; q < nbuf; q++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
      buf.write_response();
    }
    if (stamp) s2 = stamp_of(sreq, stick, pi + 1);
    // ORDERING: the doorbell must not leave before the WQE/DBR write acknowledgements. They are on different m_axi
    // ports, so HLS sees no dependence and MAY hoist the doorbell above the ack loop: the e52z build did exactly that
    // (schedule: doorbell in the state before the ack call), and the NIC then executed a slot still holding the
    // PREVIOUS lap's WQE (error CQE opcode 13 / syndrome 0x02 / vendor 0x68). ap_wait() pins the order (verified in
    // the HLS schedule; a `protocol fixed` region is rejected because the ack loop has variable latency).
    ap_wait();
    if (bf && nw == 1) {
      bfw.write_request((db_off + bfsel) / 64, lastbbs);
    bfc:
      for (unsigned q = 0; q < lastbbs; q++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=4
        bfw.write(lw[q]);
      }
      bfsel ^= bf_size;
    } else {
      bfw.write_request(db_off / 64, 1);
      bfw.write(lw[0], 0xFF);                                       // 8-byte doorbell: the last WQE's ctrl
    }
    batches++;
    ap_uint<64> s3 = 0;
    if (stamp) s3 = stamp_of(sreq, stick, bfsel + frames);
    // the PREVIOUS doorbell/BlueFlame write's acknowledgement, collected only after this one is issued (E5.1c: when
    // it was collected first, "doorbell issued" took 0.32 us)
    if (bf_out) bfw.write_response();
    bf_out = true;
    if (stamp) {
      if (log_out) { tlog.write_response(); log_out--; }
      beat_t lr = 0; lr.range(63, 0) = s0; lr.range(127, 64) = s1; lr.range(191, 128) = s2; lr.range(255, 192) = s3;
      lr.range(287, 256) = frames - 1;
      tlog.write_request((frames - 1) & 4095, 1); tlog.write(lr); log_out++;
    }
  }
  if (bf_out) bfw.write_response();
  if (log_out) tlog.write_response();
  res[0] = frames; res[1] = pi; res[2] = inl; res[3] = bufd; res[4] = waits; res[5] = polls; res[6] = batches;
}

// ------------------------------------------------------------------------------------------------ TX completion reaper
// res: [0] TX CQEs, [1] error CQEs, [2] first error word, [3] polls, [5] SQ slots zeroed
// ZERO-ON-REAP (2026-10-01): with log_sq != 0, every completed SQ slot is zeroed BEFORE its credit goes to fld_tx.
// The ConnectX was measured executing a WQE with the PREVIOUS lap's slot contents (CQE syndrome 0x02, vendor 0x68 =
// WQE violation) when a doorbell arrives as it finishes the previous WQE; first-lap (zero) slots never fail. The
// credit now frees through the signaled WQE's FIRST WQEBB (wc + 1): its remaining WQEBBs are zeroed and freed at the
// next completion, so fld_tx can never be writing a slot that is about to be zeroed.
extern "C" void fld_reap(hls::burst_maxi<beat_t> cq, uint32_t *res, hls::stream<cred_t> &cred,
                         unsigned tcq_off, unsigned log_tcq, unsigned tcq_dbr_off, unsigned stop_off, unsigned gap,
                         unsigned sq_off, unsigned log_sq)
{
#pragma HLS INTERFACE m_axi port=cq  offset=slave bundle=gmem0 depth=65536 max_read_burst_length=2 max_write_burst_length=2 num_write_outstanding=32 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem1 depth=16 latency=1
#pragma HLS INTERFACE axis port=cred
  const unsigned tcq_mask = (1u << log_tcq) - 1;
  unsigned tci = 0, errs = 0, err0 = 0, polls = 0, idle = 0, pend = 0, spin = 0, zs = 0, zeroed = 0;
  const unsigned sq_mask = (1u << log_sq) - 1;
  bool dbr_out = false, stop = false;
main:
  while (!stop) {
#pragma HLS PIPELINE off
    polls++;
    cq.read_request(tcq_off / 64 + (tci & tcq_mask), 1);
    const beat_t c = cq.read();
    const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
    bool flush = false;
    if (opcode != 0xF && (op_own & 1) == ((tci >> log_tcq) & 1)) {
      if (opcode != 0) { if (errs == 0) err0 = (op_own << 24) | (get8(c, 55) << 16) | (get8(c, 54) << 8); errs++; }
      const unsigned wc = (get8(c, 60) << 8) | get8(c, 61);
      if (log_sq != 0) {
        // zero [zs, wc] in chunks of up to 32 single-beat writes, each chunk acknowledged before the next
        const unsigned nz = (wc + 1 - zs) & 0xffff;
      zc:
        for (unsigned base = 0; base < nz; base += 32) {
#pragma HLS PIPELINE off
          const unsigned m = nz - base < 32 ? nz - base : 32;
        zw:
          for (unsigned j = 0; j < m; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
            cq.write_request(sq_off / 64 + ((zs + base + j) & sq_mask), 1); cq.write(0);
          }
        za:
          for (unsigned j = 0; j < m; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
            cq.write_response();
          }
        }
        zeroed += nz; zs = (wc + 1) & 0xffff;
      }
      cred_t cr; cr.data = wc | (log_sq != 0 ? 0x10000u : 0u); cr.keep = -1; cr.strb = -1; cr.last = 1;
      cred.write(cr);
      tci++; pend++; idle = 0;
      flush = pend >= 16;
    } else {
      flush = pend != 0;
      if (++idle >= 4096) {
        idle = 0;
        cq.read_request(stop_off / 64, 1);
        const beat_t s = cq.read();
        if (s.range(31, 0) != 0) stop = true;
      }
    gp:
      for (unsigned g = 0; g < gap; g++) {                          // back off: leave the gate to the RX/TX engines
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=1024
        spin++;                                                     // consumed in res[4], so HLS keeps the loop
      }
    }
    if (flush) {
      if (dbr_out) cq.write_response();
      beat_t d = 0; put_be32(d, 0, tci & 0xffffff);
      cq.write_request(tcq_dbr_off / 64, 1); cq.write(d);
      dbr_out = true; pend = 0;
    }
  }
  if (dbr_out) cq.write_response();
  res[0] = tci; res[1] = errs; res[2] = err0; res[3] = polls; res[4] = spin; res[5] = zeroed;
}

// ------------------------------------------------------------------------------------------------ E5.1b ticker
// Free-running cycle counter shared by fld_rx and fld_tx (two request/answer pairs), so both engines stamp on one clock.
extern "C" void e5_ticker2(hls::stream<req_t> &rq0, hls::stream<tick_t> &tk0, hls::stream<req_t> &rq1, hls::stream<tick_t> &tk1)
{
#pragma HLS INTERFACE axis port=rq0
#pragma HLS INTERFACE axis port=tk0
#pragma HLS INTERFACE axis port=rq1
#pragma HLS INTERFACE axis port=tk1
#pragma HLS INTERFACE ap_ctrl_none port=return
  ap_uint<64> c = 0;
tk:
  while (1) {
#pragma HLS PIPELINE II=1
    c++;
    req_t r;
    tick_t t; t.data = c; t.keep = -1; t.strb = -1; t.last = 1;
    if (rq0.read_nb(r)) tk0.write(t);
    if (rq1.read_nb(r)) tk1.write(t);
  }
}
