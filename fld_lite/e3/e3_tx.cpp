// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 transmit kernel (2026-09-29, needs the E3 dev shell): the kernel alone drives a ConnectX-5 send queue.
// The SQ ring, its doorbell record, the TX CQ and the frames live in one XRT P2P BO in bank0 (FPGA DDR; the
// ConnectX reaches it through BAR4). For every frame the kernel builds the 64-byte SEND WQE itself (ctrl segment,
// 18 bytes of inlined L2 header read from the frame, data segment for the rest), writes it and the SQ doorbell record
// to DDR, and then RINGS THE DOORBELL: one 8-byte PCIe write to the ConnectX UAR page through the shell's PEER port
// (peer_window -> XDMA S_AXI_B). It polls the TX CQ (also in DDR) to recycle ring slots. No CPU in the data path.
//
// mem  : the P2P BO as 64-byte beats (volatile: WQE/CQE/doorbell-record accesses are real, ordered DDR accesses)
// peer : the PEER BO (never synced); peer[db_off/8] lands on the UAR doorbell register through the host-set window
// res  : res[0] WQEs posted, res[1] CQEs consumed, res[2] error CQEs, res[3] polls, res[4] first error syndrome word
#include <ap_int.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
static inline void put8(beat_t &b, unsigned k, unsigned v) { b.range(8 * k + 7, 8 * k) = v & 0xff; }
static inline unsigned get8(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }

extern "C" void e3_tx(volatile beat_t *mem, volatile uint64_t *peer, uint32_t *res,
                      unsigned sq_off, unsigned log_sq, unsigned sq_dbr_off, unsigned cq_off, unsigned log_cq,
                      unsigned cq_dbr_off, unsigned data_off, unsigned stride, unsigned nframes, unsigned len,
                      unsigned sqn, unsigned lkey, unsigned long long data_va, unsigned db_off, unsigned n,
                      unsigned max_polls)
{
#pragma HLS INTERFACE m_axi port=mem  offset=slave bundle=gmem0 depth=131072 max_read_burst_length=2 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=peer offset=slave bundle=gmem1 depth=131072 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem2 depth=16
  const unsigned sq_size = 1u << log_sq, sq_mask = sq_size - 1, cq_mask = (1u << log_cq) - 1;
  unsigned pi = 0, ci = 0, errs = 0, polls = 0, err0 = 0;

  while ((pi < n || ci < pi) && polls < max_polls) {
    // 1. reap one completion if there is one (a CQE per WQE: every WQE asks for CQ_UPDATE)
    if (ci < pi) {
      polls++;
      const beat_t c = mem[cq_off / 64 + (ci & cq_mask)];
      const unsigned op_own = get8(c, 63), opcode = op_own >> 4;
      if (opcode != 0xF && (op_own & 1) == ((ci >> log_cq) & 1)) {
        if (opcode != 0) {                                   // 0 = MLX5_CQE_REQ; 13 = REQ_ERR
          if (errs == 0) err0 = (op_own << 24) | (get8(c, 55) << 16) | (get8(c, 54) << 8) | get8(c, 53);
          errs++;
        }
        ci++;
        beat_t cd = mem[cq_dbr_off / 64];                    // CQ set_ci (be32, 24 bits) at +0 of its beat
        put8(cd, 0, ci >> 24); put8(cd, 1, ci >> 16); put8(cd, 2, ci >> 8); put8(cd, 3, ci);
        cd.range(7, 0) = 0;                                  // set_ci is 24-bit: top byte stays 0
        mem[cq_dbr_off / 64] = cd;
      }
    }
    // 2. post one WQE if the ring has room
    if (pi < n && pi - ci < sq_size) {
      const unsigned f = pi % nframes;
      const beat_t hdr = mem[(data_off + f * stride) / 64];  // first 64 bytes of the frame
      beat_t w = 0;
      // ctrl segment: opmod_idx_opcode (be32) = wqe_index << 8 | SEND(0x0a); qpn_ds (be32) = sqn << 8 | ds(4)
      put8(w, 0, 0);  put8(w, 1, pi >> 8); put8(w, 2, pi); put8(w, 3, 0x0a);
      put8(w, 4, sqn >> 16); put8(w, 5, sqn >> 8); put8(w, 6, sqn); put8(w, 7, 4);
      put8(w, 11, 0x08);                                     // fm_ce_se: CQ_UPDATE
      // eth segment at 16: inline_hdr_sz (be16) = 18 at bytes 28..29, inline header bytes at 30..47
      put8(w, 28, 0); put8(w, 29, 18);
      for (int k = 0; k < 18; k++) put8(w, 30 + k, get8(hdr, k));
      // data segment at 48: byte_count (be32), lkey (be32), addr (be64) = frame VA + 18
      const unsigned bc = len - 18;
      const unsigned long long a = data_va + (unsigned long long)f * stride + 18;
      put8(w, 48, bc >> 24); put8(w, 49, bc >> 16); put8(w, 50, bc >> 8); put8(w, 51, bc);
      put8(w, 52, lkey >> 24); put8(w, 53, lkey >> 16); put8(w, 54, lkey >> 8); put8(w, 55, lkey);
      for (int k = 0; k < 8; k++) put8(w, 56 + k, a >> (8 * (7 - k)));
      mem[sq_off / 64 + (pi & sq_mask)] = w;                 // WQE -> FPGA DDR
      pi++;
      // SQ doorbell record (be32 at dbr[1] = +4 within its 64-byte beat): read-modify-write of that beat
      beat_t d = mem[sq_dbr_off / 64];
      put8(d, 4, 0); put8(d, 5, 0); put8(d, 6, pi >> 8); put8(d, 7, pi);
      mem[sq_dbr_off / 64] = d;
      (void)mem[sq_off / 64 + ((pi - 1) & sq_mask)];         // read back the WQE: its write has landed before the doorbell
      uint64_t db = 0;
      for (int k = 0; k < 8; k++) db |= (uint64_t)get8(w, k) << (8 * k);
      peer[db_off / 8] = db;                                  // RING: 8-byte PCIe write to the ConnectX UAR
    }
  }
  res[0] = pi; res[1] = ci; res[2] = errs; res[3] = polls; res[4] = err0;
}
