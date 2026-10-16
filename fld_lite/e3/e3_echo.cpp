// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 echo kernel (2026-09-29, E3 dev shell): receive and transmit through the ConnectX-5 with no CPU in the data path.
//   RX: the ConnectX writes frames (EtherType 0x88B6) into RX buffers in FPGA DDR and a CQE into the RX CQ (DDR).
//   TX: for every RX CQE the kernel builds a SEND WQE whose inlined L2 header is the received one with the EtherType
//       and source MAC rewritten, and whose data segment points at the SAME RX buffer (zero copy). It writes the WQE
//       and the SQ doorbell record to DDR and rings the ConnectX doorbell through the PEER port, once per batch.
//   Recycling: RX slot i is re-posted (RQ doorbell record = tx_ci + rq_size) only after TX CQE i, so the ConnectX never
//       overwrites a buffer that is still being transmitted. Every ring and doorbell record lives in DDR.
// res: [0] frames received, [1] WQEs posted, [2] TX CQEs, [3] RX error CQEs, [4] TX error CQEs, [5] doorbells, [6] polls,
//      [7] first TX error word (op_own<<24 | syndrome<<16 | vendor<<8)
#include <ap_int.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
static inline void put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline void put_be32(beat_t &b, unsigned k, unsigned v) { put8(b, k, v >> 24); put8(b, k + 1, v >> 16); put8(b, k + 2, v >> 8); put8(b, k + 3, v); }

extern "C" void e3_echo(volatile beat_t *mem, volatile uint64_t *peer, uint32_t *res,
                        unsigned rq_dbr_off, unsigned log_rq, unsigned rcq_off, unsigned rcq_dbr_off, unsigned log_rcq,
                        unsigned data_off, unsigned stride, unsigned long long data_va,
                        unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned tcq_off, unsigned tcq_dbr_off,
                        unsigned log_tcq, unsigned sqn, unsigned lkey, unsigned db_off, unsigned new_etype,
                        unsigned n, unsigned max_polls, unsigned stop_off)
{
#pragma HLS INTERFACE m_axi port=mem  offset=slave bundle=gmem0 depth=131072 max_read_burst_length=2 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=peer offset=slave bundle=gmem1 depth=131072 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem2 depth=16
  const unsigned rq_size = 1u << log_rq, rq_mask = rq_size - 1, rcq_mask = (1u << log_rcq) - 1;
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, tcq_mask = (1u << log_tcq) - 1;
  unsigned rx = 0, pi = 0, tci = 0, rxerr = 0, txerr = 0, dbs = 0, polls = 0, idle = 0, err0 = 0;
  bool stop = false;

  while (tci < n && polls < max_polls && !stop) {
    polls++;
    bool progress = false;
    // ---- RX: take up to 4 new frames (bounded by SQ room), build their WQEs, ring once ----
    uint64_t db = 0;
    bool have_db = false;
    for (int j = 0; j < 4; j++) {
      if (rx >= n || pi - tci >= sq_size) break;
      const beat_t c = mem[rcq_off / 64 + (rx & rcq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode == 0xF || (op_own & 1) != ((rx >> log_rcq) & 1)) break;
      const unsigned slot = ((get8(c, 60) << 8) | get8(c, 61)) & rq_mask;
      const unsigned len = (get8(c, 46) << 8) | get8(c, 47);
      rx++;
      beat_t rcd = 0; put_be32(rcd, 0, rx & 0xffffff);          // RX CQ set_ci
      mem[rcq_dbr_off / 64] = rcd;
      if (opcode != 2 || len < 64) { rxerr++; continue; }        // not a good RESP_SEND: drop (slot re-posted below)
      const beat_t hdr = mem[(data_off + slot * stride) / 64];
      beat_t w = 0;
      put8(w, 0, 0); put8(w, 1, pi >> 8); put8(w, 2, pi); put8(w, 3, 0x0a);   // SEND, wqe index pi
      put8(w, 4, sqn >> 16); put8(w, 5, sqn >> 8); put8(w, 6, sqn); put8(w, 7, 4);
      put8(w, 11, 0x08);                                          // CQ_UPDATE
      put8(w, 28, 0); put8(w, 29, 18);                            // inline_hdr_sz = 18
      for (int k = 0; k < 18; k++) put8(w, 30 + k, get8(hdr, k));
      put8(w, 30 + 6, 0x02); put8(w, 30 + 11, 0xEC);              // src MAC 02:..:EC -- rewritten
      put8(w, 30 + 12, new_etype >> 8); put8(w, 30 + 13, new_etype);   // EtherType rewritten
      const unsigned long long a = data_va + (unsigned long long)slot * stride + 18;
      put_be32(w, 48, len - 18); put_be32(w, 52, lkey);
      for (int k = 0; k < 8; k++) put8(w, 56 + k, a >> (8 * (7 - k)));
      mem[sq_off / 64 + (pi & sq_mask)] = w;
      pi++;
      db = 0;
      for (int k = 0; k < 8; k++) db |= (uint64_t)get8(w, k) << (8 * k);
      have_db = true;
      progress = true;
    }
    if (have_db) {
      beat_t d = 0; put_be32(d, 4, pi & 0xffff);                  // SQ doorbell record (dbr[1])
      mem[sq_dbr_off / 64] = d;
      (void)mem[sq_off / 64 + ((pi - 1) & sq_mask)];              // the last WQE has landed before the doorbell
      peer[db_off / 8] = db;                                      // ring: the ConnectX fetches WQEs up to pi
      dbs++;
    }
    // ---- TX completions: free SQ slots and give the RX buffers back to the ConnectX ----
    for (int j = 0; j < 4; j++) {
      if (tci >= pi) break;
      const beat_t c = mem[tcq_off / 64 + (tci & tcq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode == 0xF || (op_own & 1) != ((tci >> log_tcq) & 1)) break;
      if (opcode != 0) { if (txerr == 0) err0 = (op_own << 24) | (get8(c, 55) << 16) | (get8(c, 54) << 8); txerr++; }
      tci++;
      progress = true;
    }
    if (progress) {
      beat_t tcd = 0; put_be32(tcd, 0, tci & 0xffffff);          // TX CQ set_ci
      mem[tcq_dbr_off / 64] = tcd;
      // RX slots whose frame was dropped (rxerr) never got a WQE; count them as done so their buffers come back
      beat_t rqd = 0; put_be32(rqd, 0, (tci + rxerr + rq_size) & 0xffff);
      mem[rq_dbr_off / 64] = rqd;
      idle = 0;
    } else if (++idle >= 4096) {
      idle = 0;
      const beat_t s = mem[stop_off / 64];
      if (s.range(31, 0) != 0) stop = true;
    }
  }
  res[0] = rx; res[1] = pi; res[2] = tci; res[3] = rxerr; res[4] = txerr; res[5] = dbs; res[6] = polls; res[7] = err0;
}
