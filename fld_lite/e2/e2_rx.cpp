// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E2 receive kernel (2026-09-29): the ConnectX-5 receives Ethernet frames straight into FPGA DDR -- RQ ring, RQ
// doorbell record, CQ ring and CQ doorbell record all live in one XRT P2P buffer object in bank0 -- and this kernel is
// the whole receive path: it polls the CQ for new CQEs, checksums each frame, and re-posts the RQ slot by advancing the
// RQ doorbell record, which the ConnectX reads from FPGA memory. No PCIe writes and no CPU in the data path.
//
// mem   : the P2P BO (volatile: every CQE/doorbell access is a real, ordered DDR access -- no bursts, no caching)
// data  : the same BO through a second port, for burst reads of frame payloads
// Offsets are byte offsets into the BO. CQEs are 64 B; big-endian fields per the mlx5 PRM:
//   byte_cnt at CQE+44 (be32), wqe_counter at CQE+60 (be16), op_own at CQE+63 (opcode<<4 | owner).
// res   : res[0] frames consumed, res[1] polls, res[2] error/unexpected CQEs, res[3] last byte_cnt,
//         res[4+2i] = 32-bit little-endian word sum of the i-th frame consumed, res[5+2i] = its frame id (payload
//         bytes 15..16), so the host can check every consumed frame and count drops under overload.
// stop  : byte offset of a word in mem the host sets non-zero when it has finished sending (clean exit on drops).
#include <stdint.h>

static uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

extern "C" void e2_rx(volatile uint32_t *mem, const uint32_t *data, uint32_t *res,
                      unsigned cq_off, unsigned log_cq, unsigned cq_dbr_off, unsigned rq_dbr_off, unsigned log_rq,
                      unsigned data_off, unsigned stride, unsigned n, unsigned max_polls, unsigned stop_off)
{
#pragma HLS INTERFACE m_axi port=mem  offset=slave bundle=gmem0 depth=1048576 max_read_burst_length=2 max_write_burst_length=2
#pragma HLS INTERFACE m_axi port=data offset=slave bundle=gmem1 depth=1048576 max_read_burst_length=64
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem2 depth=65536
  const unsigned cq_mask = (1u << log_cq) - 1, rq_size = 1u << log_rq, rq_mask = rq_size - 1;
  unsigned ci = 0, got = 0, errs = 0, last = 0, polls = 0;

  unsigned idle = 0;
  while (got < n && polls < max_polls) {
    polls++;
    const unsigned cqe = (cq_off + (ci & cq_mask) * 64) / 4;
    const uint32_t w15 = mem[cqe + 15];                   // bytes 60..63
    const unsigned op_own = w15 >> 24, opcode = op_own >> 4, owner = op_own & 1, sw_owner = (ci >> log_cq) & 1;
    if (opcode == 0xF || owner != sw_owner) {             // not written by the ConnectX yet
      if (++idle >= 4096) {                               // quiet for a while: has the host finished sending?
        idle = 0;
        if (mem[stop_off / 4])
          break;
      }
      continue;
    }
    idle = 0;
    const unsigned byte_cnt = bswap32(mem[cqe + 11]);
    const unsigned wqe_ctr = ((w15 & 0xff) << 8) | ((w15 >> 8) & 0xff);
    uint32_t sum = 0, id = 0xFFFFFFFFu;
    if (opcode == 2) {                                    // MLX5_CQE_RESP_SEND
      unsigned words = (byte_cnt + 3) / 4;
      if (words > stride / 4)
        words = stride / 4;
      const unsigned b = (data_off + (wqe_ctr & rq_mask) * stride) / 4;
    sum_loop:
      for (unsigned k = 0; k < words; k++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=256
        sum += data[b + k];
      }
      id = (data[b + 3] >> 24) | ((data[b + 4] & 0xff) << 8);   // bytes 15 (id lo) and 16 (id hi)
      last = byte_cnt;
    } else {
      errs++;
      sum = 0xEEEE0000u | op_own;
    }
    res[4 + 2 * got] = sum;
    res[5 + 2 * got] = id;
    got++;
    ci++;
    mem[cq_dbr_off / 4] = bswap32(ci & 0xffffff);          // CQ set_ci
    mem[rq_dbr_off / 4] = bswap32((ci + rq_size) & 0xffff); // re-post the consumed RQ slot
    res[0] = got;
  }
  res[0] = got;
  res[1] = polls;
  res[2] = errs;
  res[3] = last;
}
