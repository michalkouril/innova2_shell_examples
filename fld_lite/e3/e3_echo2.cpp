// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 echo kernel v2 (2026-09-30): e3_echo plus the two optimisations that remove the ConnectX's PCIe reads of FPGA
// memory from the echo's critical path.
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

typedef ap_uint<512> beat_t;
static inline void put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline void put_be32(beat_t &b, unsigned k, unsigned v) { put8(b, k, v >> 24); put8(b, k + 1, v >> 16); put8(b, k + 2, v >> 8); put8(b, k + 3, v); }

extern "C" void e3_echo2(volatile beat_t *mem, volatile uint64_t *peer, uint32_t *res,
                         unsigned rq_dbr_off, unsigned log_rq, unsigned rcq_off, unsigned rcq_dbr_off, unsigned log_rcq,
                         unsigned data_off, unsigned stride, unsigned long long data_va,
                         unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned tcq_off, unsigned tcq_dbr_off,
                         unsigned log_tcq, unsigned sqn, unsigned lkey, unsigned db_off, unsigned new_etype,
                         unsigned n, unsigned max_polls, unsigned stop_off, unsigned bf, unsigned inline_max, unsigned bf_size)
{
#pragma HLS INTERFACE m_axi port=mem  offset=slave bundle=gmem0 depth=131072 max_read_burst_length=4 max_write_burst_length=4
#pragma HLS INTERFACE m_axi port=peer offset=slave bundle=gmem1 depth=131072 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem2 depth=16
  const unsigned rq_size = 1u << log_rq, rq_mask = rq_size - 1, rcq_mask = (1u << log_rcq) - 1;
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, tcq_mask = (1u << log_tcq) - 1;
  unsigned rx = 0, pi = 0, tci = 0, twq = 0, rxerr = 0, txerr = 0, dbs = 0, polls = 0, idle = 0, err0 = 0, inl = 0;
  unsigned bfsel = 0;
  bool stop = false;
  // per-WQE WQEBB count, to free SQ space in order as TX CQEs arrive (one CQE per WQE)
  unsigned char wqebbs_of[1024];

  while (tci < n && polls < max_polls && !stop) {
    polls++;
    bool progress = false;
    // ---- RX: one frame per iteration (lowest latency: build and ring immediately) ----
    if (rx < n && ((pi - twq) & 0xffff) + 4 <= sq_size) {
      const beat_t c = mem[rcq_off / 64 + (rx & rcq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode != 0xF && (op_own & 1) == ((rx >> log_rcq) & 1)) {
        const unsigned slot = ((get8(c, 60) << 8) | get8(c, 61)) & rq_mask;
        const unsigned len = (get8(c, 46) << 8) | get8(c, 47);
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
        wr:
          for (unsigned k = 0; k < 4; k++) if (k < bbs) mem[sq_off / 64 + ((pi + k) & sq_mask)] = w[k];
          wqebbs_of[pi & 1023] = bbs;
          pi += bbs;
          beat_t d = 0; put_be32(d, 4, pi & 0xffff);                     // SQ doorbell record
          mem[sq_dbr_off / 64] = d;
          (void)mem[sq_off / 64 + ((pi - 1) & sq_mask)];                // WQE landed before the doorbell
          if (bf) {
            // BlueFlame: the whole WQE into the current BlueFlame buffer, 8 bytes at a time, in order
          bfc:
            for (unsigned q = 0; q < 32; q++) {
#pragma HLS PIPELINE II=1
              if (q < bbs * 8) {
                uint64_t v = 0;
                for (int b = 0; b < 8; b++) v |= (uint64_t)get8(w[q / 8], (q % 8) * 8 + b) << (8 * b);
                peer[(db_off + bfsel + q * 8) / 8] = v;
              }
            }
            bfsel ^= bf_size;
          } else {
            uint64_t db = 0;
            for (int k = 0; k < 8; k++) db |= (uint64_t)get8(w[0], k) << (8 * k);
            peer[db_off / 8] = db;
          }
          dbs++;
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
  res[0] = rx; res[1] = pi; res[2] = tci; res[3] = rxerr; res[4] = txerr; res[5] = dbs; res[6] = polls; res[7] = err0; res[8] = inl;
}
