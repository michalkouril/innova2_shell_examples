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
// BUILD NOTE (2026-10-03, "BRAM diet"): compile every kernel in this file with
//   v++ -c ... --hls.pre_tcl fld_lite_hls.tcl        (config_interface -m_axi_buffer_impl auto)
// and keep the m_axi pragmas below (unused directions minimal, 32-bit res ports). Together: 8.5 BRAM36 instead of 45.5,
// and faster (idle echo 64 B 1.94 vs 2.17 us, +10 % frames/s at 192-512 B; E5.5 A/B/A on fp6, hpy440).
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
// m_axi latency=1 everywhere: with the default (64) HLS schedules every request/response as a FIXED ~70-cycle
// operation and pads to it even when the data is back in ~49 cycles; latency=1 waits on the handshake instead.
// Measured (e51LA): FPGA echo 2.26 -> 1.95 us at 64 B, 3.85 -> 3.35 us at 1024 B.
// E5.3 BURST POLLING (2026-10-02): pre = npre | (K << 8). Each poll reads K consecutive CQEs in ONE request (never
// across the ring end) plus npre (<= 2 when K > 1) speculative beats of each of the K expected slots, all on the same
// port behind the CQEs (RX ORDERING holds for every one). The leading run of valid CQEs is consumed together: the
// remainder requests of all of them are issued back to back, then the frames are streamed in order. One round trip
// per poll then serves up to K frames instead of one; under load the RQ no longer runs dry at ~1.1 Mfps. K = 0/1 is
// the E5.1c engine exactly (npre up to 4). An idle poll costs K + K*npre beats of the gate instead of 1 + npre.
// E5.5 ADAPTIVE K (2026-10-02): a poll after an empty poll reads ONE CQE (+ npre beats), a poll after one that found
// work reads K. An idle engine then polls as cheaply as the K=1 engine (fp4, 64 B echo: K=8 2.38 us, K=4 2.18, K=1 1.86)
// and a burst pays at most one single-CQE poll before batching. pre bit 16 pins K (E5.3 behaviour) for comparisons.
// res: [0] CQEs, [1] frames streamed, [2] RX error CQEs, [3] slot mismatches (speculative read wasted), [4] polls,
//      [5] polls that found >1 CQE, [6] largest run consumed by one poll, [7] polls issued with K > 1
extern "C" void fld_rx(hls::burst_maxi<beat_t> mem, hls::burst_maxi<beat_t> dbr, uint32_t *res,
                       hls::stream<pkt_t> &rxs,
                       unsigned rcq_off, unsigned log_rcq, unsigned rcq_dbr_off, unsigned rq_dbr_off, unsigned log_rq,
                       unsigned data_off, unsigned stride, unsigned stop_off, unsigned batch,
                       unsigned pre, unsigned stamp, hls::burst_maxi<beat_t> tlog,
                       hls::stream<req_t> &sreq, hls::stream<tick_t> &stick)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=131072 max_read_burst_length=32 num_read_outstanding=16 max_write_burst_length=2 num_write_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=dbr offset=slave bundle=gmem1 depth=65536 max_write_burst_length=2 max_read_burst_length=2 num_write_outstanding=4 num_read_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem2 depth=16 max_widen_bitwidth=32 max_read_burst_length=2 max_write_burst_length=16 num_read_outstanding=1 num_write_outstanding=1 latency=1
#pragma HLS INTERFACE axis port=rxs
#pragma HLS INTERFACE m_axi port=tlog offset=slave bundle=gmem3 depth=65536 max_write_burst_length=2 max_read_burst_length=2 num_read_outstanding=1 num_write_outstanding=4 latency=1
#pragma HLS INTERFACE axis port=sreq
#pragma HLS INTERFACE axis port=stick
  const unsigned KMAX = 8;
  const unsigned rcq_size = 1u << log_rcq, rcq_mask = rcq_size - 1, rq_size = 1u << log_rq, rq_mask = rq_size - 1;
  const unsigned kreq = (pre >> 8) & 0xff, kk = kreq == 0 ? 1 : kreq > KMAX ? KMAX : kreq;
  const unsigned pre0 = pre & 0xff, npre = kk == 1 ? (pre0 > 4 ? 4 : pre0) : (pre0 > 2 ? 2 : pre0);
  const bool kfixed = (pre >> 16) & 1;
  // UDP step 1 (2026-10-03): pre bit 17 = drop frames whose CQE reports a bad IP or L4 checksum (hds_ip_ext, byte 28:
  // L3_OK bit 1, L4_OK bit 2) or IP options (l4_l3_hdr_type, byte 29, bit 1) -- counted in res[8]. Off by default.
  const bool rxchk = (pre >> 17) & 1;
  unsigned csdrop = 0;
  unsigned rx = 0, frames = 0, errs = 0, mism = 0, polls = 0, multi = 0, maxrun = 0, pend = 0, idle = 0, kpolls = 0;
  bool busy = false;
  bool dbr_out = false, stop = false;
  unsigned log_out = 0;
  beat_t c[KMAX], fb[KMAX * 4];
  unsigned short flen[KMAX], fnb[KMAX], fhave[KMAX];
  bool fok[KMAX];

main:
  while (!stop) {
#pragma HLS PIPELINE off
    polls++;
    const unsigned kc = (kfixed || busy) ? kk : 1;
    const unsigned at = rx & rcq_mask, room = rcq_size - at, k = kc < room ? kc : room;
    if (k > 1) kpolls++;
    // the K CQEs and, right behind them on the same port, the slots they will normally point at (see RX ORDERING)
    mem.read_request(rcq_off / 64 + at, k);
  rqp:
    for (unsigned j = 0; j < k; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=8
      if (npre) mem.read_request((data_off + ((rx + j) & rq_mask) * stride) / 64, npre);
    }
  rdc:
    for (unsigned j = 0; j < k; j++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=8
      c[j] = mem.read();
    }
  rdf:
    for (unsigned i = 0; i < k * npre; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=16
      const unsigned j = npre == 1 ? i : npre == 2 ? i >> 1 : i >> 2, q = npre == 1 ? 0 : npre == 2 ? i & 1 : i & 3;
      fb[j * 4 + q] = mem.read();
    }
    // leading run of valid CQEs; for each, its remainder request (one per frame, in order)
    unsigned n = 0;
    bool run = true;
  vc:
    for (unsigned j = 0; j < k; j++) {
#pragma HLS PIPELINE off
#pragma HLS LOOP_TRIPCOUNT max=8
      const unsigned op_own = get8(c[j], 63), opcode = op_own >> 4;
      run = run && opcode != 0xF && (op_own & 1) == (((rx + j) >> log_rcq) & 1);
      if (run) {
        const unsigned slot = ((get8(c[j], 60) << 8) | get8(c[j], 61)) & rq_mask;
        const unsigned len = (get8(c[j], 46) << 8) | get8(c[j], 47);
        const bool hw_ok = !rxchk || ((get8(c[j], 28) & 6) == 6 && !(get8(c[j], 29) & 2));
        const bool ok = opcode == 2 && len >= 60 && len <= stride && hw_ok;
        if (opcode == 2 && !hw_ok) csdrop++;
        const unsigned nb = (len + 63) / 64;
        unsigned have = npre;
        if (slot != ((rx + j) & rq_mask)) { mism += npre != 0; have = 0; }
        if (have > nb) have = nb;
        if (ok && nb > have) mem.read_request((data_off + slot * stride) / 64 + have, nb - have);   // the rest
        if (!ok) errs++;
        fok[j] = ok; flen[j] = len; fnb[j] = nb; fhave[j] = have;
        n++;
      }
    }
    bool flush = false;
    if (n != 0) {
      ap_uint<64> t0 = 0;
      if (stamp) t0 = stamp_of(sreq, stick, get8(c[0], 63));
    fr:
      for (unsigned j = 0; j < n; j++) {
#pragma HLS PIPELINE off
#pragma HLS LOOP_TRIPCOUNT max=8
        if (fok[j]) {
          const unsigned len = flen[j], nb = fnb[j], have = fhave[j];
        out:
          for (unsigned q = 0; q < nb; q++) {                       // variable bound: a constant bound with an
#pragma HLS PIPELINE II=1                                           // if() runs every iteration (E5.1c lesson)
#pragma HLS LOOP_TRIPCOUNT max=130
            pkt_t p;
            p.data = q < have ? fb[j * 4 + (q & 3)] : mem.read();
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
            beat_t lr = 0; lr.range(63, 0) = t0; lr.range(127, 64) = t1; lr.range(159, 128) = rx + j;
            tlog.write_request((rx + j) & 4095, 1); tlog.write(lr); log_out++;
          }
        }
      }
      if (n > 1) multi++;
      if (n > maxrun) maxrun = n;
      rx += n; pend += n; idle = 0; busy = true;
      flush = pend >= batch;
    } else {
      busy = false;
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
  res[0] = rx; res[1] = frames; res[2] = errs; res[3] = mism; res[4] = polls; res[5] = multi; res[6] = maxrun; res[7] = kpolls; res[8] = csdrop;
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
// E5.4 LINGER (2026-10-02): maxb = bmax | (linger << 8). When the previous batch held more than one frame (the engine is
// under load), a batch that would close below bmax first waits up to `linger` cycles for another frame; a lone frame
// on an idle engine never waits. Fewer, larger batches = fewer acknowledgement round trips (~0.3 us each) per frame.
// res: [0] frames sent, [1] WQEBBs, [2] inline frames, [3] buffered frames, [4] credit waits, [5] polls, [6] batches,
//      [7] frames that joined a batch during a linger, [8] largest batch, [9] batches closed early for room (carry)
// E5.5 (2026-10-02):
// * ADAPTIVE INLINE: inline_max = cap | (1 << 16). With bit 16 set only the FIRST frame of a batch, and only when the
//   previous batch held one frame (idle link), is sent inline; every other frame goes by pointer. Measured on fp4:
//   inline saves 0.7-0.8 us per lone frame (no NIC read of the buffer), but under load the pointer path is 1.1-1.6x
//   faster (128 B: 1.92 M fps inline 222 vs 3.04 M by pointer) -- inline WQEs take 2-4 WQEBBs each.
// * NO MID-BATCH CREDIT WAIT: a frame that finds no SQ room / buffer slot while the batch already holds WQEs is
//   CARRIED to the next batch, after this batch's doorbell. Blocking there could deadlock: the credits it waits for
//   may belong to this batch's own, not yet doorbelled, WQEs (seen on fp4: 192 B inline 222 hung at rxk 4, ran at rxk 1).
extern "C" void fld_tx(hls::burst_maxi<beat_t> sq, hls::burst_maxi<beat_t> buf, hls::burst_maxi<beat_t> bfw, uint32_t *res,
                       hls::stream<pkt_t> &txs, hls::stream<cred_t> &cred,
                       unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned sqn, unsigned lkey,
                       unsigned buf_off, unsigned long long buf_va, unsigned log_nslots, unsigned slot_stride,
                       unsigned db_off, unsigned bf, unsigned bf_size, unsigned inline_max, unsigned sig_every,
                       unsigned stop_off, unsigned stamp, hls::burst_maxi<beat_t> tlog,
                       hls::stream<req_t> &sreq, hls::stream<tick_t> &stick, unsigned maxb)
{
#pragma HLS INTERFACE m_axi port=sq  offset=slave bundle=gmem0 depth=65536 max_write_burst_length=2 max_read_burst_length=2 num_write_outstanding=32 num_read_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=buf offset=slave bundle=gmem1 depth=131072 max_write_burst_length=32 num_write_outstanding=32 max_read_burst_length=2 num_read_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=bfw offset=slave bundle=gmem2 depth=16384 max_write_burst_length=4 max_read_burst_length=2 num_read_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem3 depth=16 max_widen_bitwidth=32 max_read_burst_length=2 max_write_burst_length=16 num_read_outstanding=1 num_write_outstanding=1 latency=1
#pragma HLS INTERFACE axis port=txs
#pragma HLS INTERFACE axis port=cred
#pragma HLS INTERFACE m_axi port=tlog offset=slave bundle=gmem4 depth=65536 max_write_burst_length=2 max_read_burst_length=2 num_read_outstanding=1 num_write_outstanding=4 latency=1
#pragma HLS INTERFACE axis port=sreq
#pragma HLS INTERFACE axis port=stick
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, nslots = 1u << log_nslots, slot_mask = nslots - 1;
  // UDP step 1: maxb bits 31:24 = Ethernet-segment cs_flags for every WQE (0xC0 = NIC computes IP + L4 checksums)
  const unsigned mb = maxb & 0xff, bmax = mb == 0 ? 1 : mb > 16 ? 16 : mb, linger = (maxb >> 8) & 0xffff, csf = (maxb >> 24) & 0xff;
  unsigned lingered = 0, bigb = 0, lastnw = 1, carries = 0;
  const unsigned imax = inline_max & 0xffff;
  const bool iadapt = (inline_max >> 16) & 1;
  bool carry = false;
  pkt_t p;
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
    if (!carry && !txs.read_nb(p)) {
      if (++idle >= 4096) {
        idle = 0;
        sq.read_request(stop_off / 64, 1);
        const beat_t s = sq.read();
        if (s.range(31, 0) != 0) stop = true;
      }
      continue;
    }
    carry = false;
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
      const bool in = imax != 0 && len <= imax && (!iadapt || (nw == 0 && lastnw <= 1));
      const unsigned ds = in ? (len + 34 + 15) >> 4 : 4, bbs = (ds + 3) >> 2, nb = (len + 63) >> 6;
      // room in the SQ and (for a buffered frame) a free buffer slot. With WQEs of this batch already written, never
      // block: take the credits that are there, and if still short close the batch and carry this frame (see E5.5).
      if (nw != 0) {
      drn:
        while (cred.read_nb(cr)) {
#pragma HLS PIPELINE off
          const unsigned wc = cr.data & 0xffff;
          twq = (wc + ((cr.data & 0x10000) ? 1u : (unsigned)bbs_of[wc & 1023])) & 0xffff;
          sfree = slots_after[wc & 1023];
        }
        if (((pi - twq) & 0xffff) + bbs > sq_size || (!in && ((sa - sfree) & 0xffff) >= nslots)) { carry = true; carries++; break; }
      }
      // first frame of a batch: every earlier WQE has been doorbelled, so waiting for credits is safe
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
      put8(w[0], 20, csf);                                          // eth segment cs_flags
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
      const bool cap = nw < bmax && nack + 5 <= 60 && nbb + 5 <= 30;
      more = cap && txs.read_nb(p);
      if (cap && !more && linger != 0 && lastnw > 1) {
      lg:
        for (unsigned g = 0; g < linger && !more; g++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=256
          more = txs.read_nb(p);
        }
        lingered += more;
      }
    }
    lastnw = nw; if (nw > bigb) bigb = nw;
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
  res[0] = frames; res[1] = pi; res[2] = inl; res[3] = bufd; res[4] = waits; res[5] = polls; res[6] = batches; res[7] = lingered; res[8] = bigb; res[9] = carries;
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
#pragma HLS INTERFACE m_axi port=cq  offset=slave bundle=gmem0 depth=65536 max_read_burst_length=2 max_write_burst_length=2 num_write_outstanding=32 num_read_outstanding=4 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem1 depth=16 max_widen_bitwidth=32 max_read_burst_length=2 max_write_burst_length=16 num_read_outstanding=1 num_write_outstanding=1 latency=1
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
