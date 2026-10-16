// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 echo kernel v5 (2026-09-30, E5.0): e3_echo4 plus a kernel-internal timestamp log.
//   A free-running ticker process (DATAFLOW sibling, II=1) answers stamp requests with its cycle count. Every request
//   carries a byte of the data just read, so a stamp cannot be taken before that read has returned.
//   Per frame, at its TX CQE, one 64-byte record goes to tlog[frame]:
//     [0] t0 RX CQE seen   [1] t1 frame read back   [2] t2 WQE+DBR written (after the fence read when fence=1)
//     [3] t3 doorbell/BlueFlame write issued   [4] t4 TX CQE seen   [5] polls at t0   [6] polls at t4   [7] frame
//   t2 is only ordered after the WQE writes when fence=1 (a write returns no data to hang the stamp on).
// E3 echo kernel v4 (2026-09-30): e3_echo3 plus
//   fence=1       a REAL read-back fence before the doorbell/BlueFlame write. Every earlier kernel wrote `(void)mem[x]`,
//                 which HLS silently drops (HLS 207-5300); here the read is assigned and consumed (res[9]).
//   poison        now independent of bf, so poison with bf=0 is the positive control (the ConnectX must fetch the
//                 corrupted ring copy: error CQEs for non-inline frames, byte mismatches for inline ones).
// E3 echo kernel v3 (2026-09-30): e3_echo2 with the BlueFlame copy sent as ONE 64-byte write per WQEBB through a
// second, 512-bit PEER port (peerw). e3_echo2's 8-byte writes reached the ConnectX as 8 separate TLPs per WQEBB and bought
// only ~0.15 us, consistent with the ConnectX ignoring them and fetching the WQE anyway.
//   poison=1 (with bf=1)  the SQ ring copy of every WQE is deliberately corrupted, the BlueFlame copy is correct: a
//                 non-inline WQE's ring lkey is wrong (a fetch gives an error CQE), an inline WQE's ring payload has one
//                 byte flipped (a fetch gives a byte mismatch). Echoes still byte-exact => the ConnectX used BlueFlame.
// Description of e3_echo2 follows.
//   bf=1          BlueFlame (WQE-by-MMIO): after writing the WQE to the SQ ring and the doorbell record, the kernel
//                 copies the whole WQE into the UAR page's BlueFlame buffer through PEER (8-byte writes, like libmlx5's
//                 mmio copy), alternating between the page's two buffers. The ConnectX then needs no WQE fetch.
//   inline_max=L  frames of at most L bytes are copied INTO the WQE (18-byte L2 header in the Ethernet segment plus an
//                 inline data segment), so the ConnectX reads nothing from FPGA memory to send them.
//                 A WQE must fit the BlueFlame buffer (bf_size, 256 B): L <= bf_size - 34.
// Zero copy otherwise (the data segment points at the RX buffer), RX slot recycled after its TX CQE, as e3_echo.
// res: [0] frames received, [1] WQEBBs posted, [2] TX CQEs, [3] RX errors, [4] TX errors, [5] doorbells/BlueFlame
//      writes, [6] polls, [7] first TX error word, [8] frames sent inline
#include <ap_int.h>
#include <stdint.h>
#include <hls_stream.h>

typedef ap_uint<512> beat_t;
static inline void put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline void put_be32(beat_t &b, unsigned k, unsigned v) { put8(b, k, v >> 24); put8(b, k + 1, v >> 16); put8(b, k + 2, v >> 8); put8(b, k + 3, v); }

static void echo_core(volatile beat_t *mem, volatile uint64_t *peer, uint32_t *res,
                         unsigned rq_dbr_off, unsigned log_rq, unsigned rcq_off, unsigned rcq_dbr_off, unsigned log_rcq,
                         unsigned data_off, unsigned stride, unsigned long long data_va,
                         unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned tcq_off, unsigned tcq_dbr_off,
                         unsigned log_tcq, unsigned sqn, unsigned lkey, unsigned db_off, unsigned new_etype,
                         unsigned n, unsigned max_polls, unsigned stop_off, unsigned bf, unsigned inline_max, unsigned bf_size,
                         unsigned poison, volatile beat_t *peerw, unsigned fence,
                      beat_t *tlog, hls::stream<ap_uint<9> > &req, hls::stream<ap_uint<64> > &tick)
{
  const unsigned rq_size = 1u << log_rq, rq_mask = rq_size - 1, rcq_mask = (1u << log_rcq) - 1;
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, tcq_mask = (1u << log_tcq) - 1;
  unsigned rx = 0, pi = 0, tci = 0, twq = 0, rxerr = 0, txerr = 0, dbs = 0, polls = 0, idle = 0, err0 = 0, inl = 0;
  unsigned bfsel = 0, fsink = 0;
  // stamp: send a data-dependent request, wait for the ticker's answer
  auto stamp = [&](unsigned dep) -> ap_uint<64> { req.write(dep & 0xFF); return tick.read(); };
  ap_uint<64> ts0[1024], ts1[1024], ts2[1024], ts3[1024]; unsigned pol0[1024];
  bool stop = false;
  // per-WQE WQEBB count, to free SQ space in order as TX CQEs arrive (one CQE per WQE)
  unsigned char wqebbs_of[1024];

  while (tci < n && polls < max_polls && !stop) {
    // not pipelined: with every inner loop constant-bound HLS otherwise pipelines this poll loop at II ~256
#pragma HLS PIPELINE off
    polls++;
    bool progress = false;
    // ---- RX: one frame per iteration (lowest latency: build and ring immediately) ----
    if (rx < n && ((pi - twq) & 0xffff) + 4 <= sq_size) {
      const beat_t c = mem[rcq_off / 64 + (rx & rcq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode != 0xF && (op_own & 1) == ((rx >> log_rcq) & 1)) {
        const unsigned slot = ((get8(c, 60) << 8) | get8(c, 61)) & rq_mask;
        const unsigned len = (get8(c, 46) << 8) | get8(c, 47);
        ts0[rx & 1023] = stamp(op_own); pol0[rx & 1023] = polls;
        rx++;
        beat_t rcd = 0; put_be32(rcd, 0, rx & 0xffffff);
        mem[rcq_dbr_off / 64] = rcd;
        progress = true;
        if (opcode != 2 || len < 64) { rxerr++; }
        else {
          const unsigned fb = (data_off + slot * stride) / 64;
          beat_t f[4];
          const bool in = (inline_max != 0) && (len <= inline_max);
          const unsigned nb = in ? (len + 63) / 64 : 1;
        rd:
          for (unsigned k = 0; k < 4; k++) if (k < nb) f[k] = mem[fb + k];
          ts1[(rx - 1) & 1023] = stamp(get8(f[nb - 1], 63));
          // rewrite the header in place in our copy
          put8(f[0], 6, 0x02); put8(f[0], 11, 0xEC); put8(f[0], 12, new_etype >> 8); put8(f[0], 13, new_etype);
          unsigned ds, bbs;
          beat_t w[4];
          for (int k = 0; k < 4; k++) w[k] = 0;
          if (in) { ds = (len + 34 + 15) / 16; }
          else    { ds = 4; }
          bbs = (ds + 3) / 4;
          put8(w[0], 0, 0); put8(w[0], 1, pi >> 8); put8(w[0], 2, pi); put8(w[0], 3, 0x0a);   // SEND at WQEBB index pi
          put8(w[0], 4, sqn >> 16); put8(w[0], 5, sqn >> 8); put8(w[0], 6, sqn); put8(w[0], 7, ds);
          put8(w[0], 11, 0x08);                                                                // CQ_UPDATE
          put8(w[0], 28, 0); put8(w[0], 29, 18);                                               // inline L2 header: 18 B
          for (int k = 0; k < 18; k++) put8(w[0], 30 + k, get8(f[0], k));
          if (in) {
            // inline data segment at byte 48: be32 (byte_count | 0x80000000), then frame bytes 18..len-1 at byte 52.
            // Frame byte k lands at WQE byte k + 34: a constant shift of the concatenated frame (pure wiring).
            put_be32(w[0], 48, (len - 18) | 0x80000000u);
            const ap_uint<2048> F = (f[3], f[2], f[1], f[0]);
            const ap_uint<2048> S = (F >> (18 * 8)) << (52 * 8);           // frame bytes 18.. -> WQE bytes 52..
            ap_uint<2048> W = (w[3], w[2], w[1], w[0]);
            W.range(2047, 52 * 8) = S.range(2047, 52 * 8);
            w[0] = W.range(511, 0); w[1] = W.range(1023, 512); w[2] = W.range(1535, 1024); w[3] = W.range(2047, 1536);
            inl++;
          } else {
            const unsigned long long a = data_va + (unsigned long long)slot * stride + 18;
            put_be32(w[0], 48, len - 18); put_be32(w[0], 52, lkey);
            for (int k = 0; k < 8; k++) put8(w[0], 56 + k, a >> (8 * (7 - k)));
          }
          // the ring copy (optionally poisoned; the BlueFlame copy below is always the correct w[])
          beat_t rw0 = w[0], rw1 = w[1];
          if (poison) {
            if (in) put8(rw1, 4, get8(w[1], 4) ^ 0xFF);                               // WQE byte 68 = frame byte 34
            else    put_be32(rw0, 52, lkey ^ 0x00A5A500);                             // a key that does not exist
          }
        wr:
          for (unsigned k = 0; k < 4; k++) if (k < bbs) mem[sq_off / 64 + ((pi + k) & sq_mask)] = k == 0 ? rw0 : k == 1 ? rw1 : w[k];
          wqebbs_of[pi & 1023] = bbs;
          pi += bbs;
          beat_t d = 0; put_be32(d, 4, pi & 0xffff);                     // SQ doorbell record
          mem[sq_dbr_off / 64] = d;
          if (fence) {                                                    // WQE landed before the doorbell
            const beat_t fb = mem[sq_off / 64 + ((pi - 1) & sq_mask)];
            fsink += fb.range(31, 0);
          }
          ts2[(rx - 1) & 1023] = stamp(fsink + pi);
          if (bf) {
            // BlueFlame: one 64-byte write per WQEBB, in order, into the current BlueFlame buffer
          bfc:
            for (unsigned k = 0; k < 4; k++) if (k < bbs) peerw[(db_off + bfsel) / 64 + k] = w[k];
            bfsel ^= bf_size;
          } else {
            uint64_t db = 0;
            for (int k = 0; k < 8; k++) db |= (uint64_t)get8(w[0], k) << (8 * k);
            peer[db_off / 8] = db;
          }
          dbs++;
          ts3[(rx - 1) & 1023] = stamp(bfsel + dbs);
        }
      }
    }
    // ---- TX completions: free SQ WQEBBs and give the RX buffers back ----
    if (tci < rx - rxerr) {
      const beat_t c = mem[tcq_off / 64 + (tci & tcq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode != 0xF && (op_own & 1) == ((tci >> log_tcq) & 1)) {
        if (opcode != 0) { if (txerr == 0) err0 = (op_own << 24) | (get8(c, 55) << 16) | (get8(c, 54) << 8); txerr++; }
        const unsigned wc = (get8(c, 60) << 8) | get8(c, 61);           // WQEBB index of the completed WQE
        twq = (wc + wqebbs_of[wc & 1023]) & 0xffff;
        {
          const ap_uint<64> t4 = stamp(op_own);
          const unsigned fi = tci & 1023;
          beat_t lr;
          lr.range(63, 0) = ts0[fi]; lr.range(127, 64) = ts1[fi]; lr.range(191, 128) = ts2[fi]; lr.range(255, 192) = ts3[fi];
          lr.range(319, 256) = t4; lr.range(383, 320) = pol0[fi]; lr.range(447, 384) = polls; lr.range(511, 448) = tci;
          tlog[tci] = lr;
        }
        tci++;
        beat_t tcd = 0; put_be32(tcd, 0, tci & 0xffffff);
        mem[tcq_dbr_off / 64] = tcd;
        beat_t rqd = 0; put_be32(rqd, 0, (tci + rxerr + rq_size) & 0xffff);
        mem[rq_dbr_off / 64] = rqd;
        progress = true;
      }
    }
    if (progress) idle = 0;
    else if (++idle >= 4096) {
      idle = 0;
      const beat_t s = mem[stop_off / 64];
      if (s.range(31, 0) != 0) stop = true;
    }
  }
  res[0] = rx; res[1] = pi; res[2] = tci; res[3] = rxerr; res[4] = txerr; res[5] = dbs; res[6] = polls; res[7] = err0; res[8] = inl; res[9] = fsink;
  req.write(0x100);                                                       // stop the ticker
}

// free-running cycle counter: answers each stamp request with the current count; 0x100 stops it
static void ticker(hls::stream<ap_uint<9> > &req, hls::stream<ap_uint<64> > &tick)
{
  ap_uint<64> c = 0;
  bool done = false;
tk:
  while (!done) {
#pragma HLS PIPELINE II=1
    c++;
    ap_uint<9> r;
    if (req.read_nb(r)) { if (r == 0x100) done = true; else tick.write(c); }
  }
}

extern "C" void e3_echo5(volatile beat_t *mem, volatile uint64_t *peer, uint32_t *res,
                         unsigned rq_dbr_off, unsigned log_rq, unsigned rcq_off, unsigned rcq_dbr_off, unsigned log_rcq,
                         unsigned data_off, unsigned stride, unsigned long long data_va,
                         unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned tcq_off, unsigned tcq_dbr_off,
                         unsigned log_tcq, unsigned sqn, unsigned lkey, unsigned db_off, unsigned new_etype,
                         unsigned n, unsigned max_polls, unsigned stop_off, unsigned bf, unsigned inline_max, unsigned bf_size,
                         unsigned poison, volatile beat_t *peerw, unsigned fence, beat_t *tlog)
{
#pragma HLS INTERFACE m_axi port=mem  offset=slave bundle=gmem0 depth=131072 max_read_burst_length=4 max_write_burst_length=4
#pragma HLS INTERFACE m_axi port=peer offset=slave bundle=gmem1 depth=131072 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem2 depth=16
#pragma HLS INTERFACE m_axi port=peerw offset=slave bundle=gmem3 depth=16384 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=tlog offset=slave bundle=gmem2 depth=4096
#pragma HLS DATAFLOW
  hls::stream<ap_uint<9> > req("req");
  hls::stream<ap_uint<64> > tick("tick");
#pragma HLS STREAM variable=req depth=2
#pragma HLS STREAM variable=tick depth=2
  echo_core(mem, peer, res, rq_dbr_off, log_rq, rcq_off, rcq_dbr_off, log_rcq, data_off, stride, data_va, sq_off, log_sq,
            sq_dbr_off, tcq_off, tcq_dbr_off, log_tcq, sqn, lkey, db_off, new_etype, n, max_polls, stop_off, bf, inline_max,
            bf_size, poison, peerw, fence, tlog, req, tick);
  ticker(req, tick);
}
