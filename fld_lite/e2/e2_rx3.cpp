// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E2 receive kernel, v3 (throughput) -- 2026-09-29. Same contract as e2_rx.cpp (ConnectX RQ/CQ rings, doorbell
// records and frame buffers in one XRT P2P BO in bank0; this kernel is the only consumer), restructured as a dataflow
// pipeline on 512-bit ports so the DDR round trips overlap instead of adding up:
//   poller   : reads 4 CQEs (one 256-byte burst) per DDR round trip, emits one command per new CQE
//   checksum : reads each frame as 64-byte beats (16 beats per 1 KB frame), sums its 32-bit LE words, extracts the id
//   writer   : one 64-bit result per frame; CQ set_ci and the RQ doorbell record written every 16 frames (and at exit)
// Each stage has its own m_axi bundle (dataflow: one process per argument); the host passes the same BO to each.
//   res64[0] = polls<<32 | frames, res64[1] = last byte_cnt<<32 | error CQEs, res64[2+i] = id<<32 | word sum
#include <ap_int.h>
#include <hls_stream.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;
struct cmd_t { ap_uint<16> slot; ap_uint<16> len; ap_uint<8> op_own; ap_uint<1> end; ap_uint<32> polls; };
struct rsl_t { ap_uint<32> sum; ap_uint<32> id; ap_uint<16> len; ap_uint<1> err; ap_uint<1> end; ap_uint<32> polls; };

static inline unsigned byte_at(const beat_t &b, unsigned k) { return b.range(8 * k + 7, 8 * k); }
static inline uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

static void poller(const beat_t *cqp, volatile const uint32_t *ctl, hls::stream<cmd_t> &out, unsigned cq_off,
                   unsigned log_cq, unsigned n, unsigned max_polls, unsigned stop_off)
{
  const unsigned mask = (1u << log_cq) - 1;
  unsigned ci = 0, polls = 0, idle = 0;
  bool done = false;
poll:
  while (!done) {
    polls++;
    const unsigned grp = (ci & mask) & ~3u;
    beat_t c[4];
  rd:
    for (int j = 0; j < 4; j++) {
#pragma HLS PIPELINE II=1
      c[j] = cqp[cq_off / 64 + grp + j];                 // one 4-beat burst
    }
    bool progress = false;
  scan:
    for (int j = 0; j < 4; j++) {
      if ((unsigned)j >= (ci & 3u) && !done) {
        const unsigned op_own = byte_at(c[j], 63), opcode = op_own >> 4, owner = op_own & 1;
        const unsigned sw_owner = (ci >> log_cq) & 1;
        if (opcode != 0xF && owner == sw_owner && ((ci & mask) & ~3u) == grp) {
          cmd_t m;
          m.slot = (byte_at(c[j], 60) << 8) | byte_at(c[j], 61);   // wqe_counter (be16)
          m.len = (byte_at(c[j], 46) << 8) | byte_at(c[j], 47);    // byte_cnt (be32, low half)
          m.op_own = op_own; m.end = 0; m.polls = 0;
          out.write(m);
          ci++;
          progress = true;
          if (ci >= n) done = true;
        } else {
          break;
        }
      }
    }
    if (progress) {
      idle = 0;
    } else if (++idle >= 4096) {                          // quiet: has the host finished sending?
      idle = 0;
      if (ctl[stop_off / 4]) done = true;
    }
    if (polls >= max_polls) done = true;
  }
  cmd_t e; e.slot = 0; e.len = 0; e.op_own = 0; e.end = 1; e.polls = polls;
  out.write(e);
}

static void checksum(const beat_t *data, hls::stream<cmd_t> &in, hls::stream<rsl_t> &out, unsigned data_off,
                     unsigned stride, unsigned log_rq)
{
  const unsigned rq_mask = (1u << log_rq) - 1;
frame:
  for (;;) {
    const cmd_t m = in.read();
    rsl_t r; r.end = m.end; r.polls = m.polls; r.len = m.len; r.sum = 0; r.id = 0xFFFFFFFFu;
    r.err = (m.op_own >> 4) != 2;                          // MLX5_CQE_RESP_SEND
    if (m.end) { out.write(r); break; }
    if (!r.err) {
      unsigned beats = ((unsigned)m.len + 63) / 64;
      if (beats > stride / 64) beats = stride / 64;
      const unsigned b = (data_off + ((unsigned)m.slot & rq_mask) * stride) / 64;
      uint32_t sum = 0, id = 0;
    beat:
      for (unsigned k = 0; k < beats; k++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
        const beat_t v = data[b + k];
        uint32_t s = 0;
        for (int w = 0; w < 16; w++) {
#pragma HLS UNROLL
          // words past byte_cnt are the RX buffer's stale tail: count only whole words inside the frame
          if (k * 64 + w * 4 + 4 <= (unsigned)m.len) s += (uint32_t)v.range(32 * w + 31, 32 * w);
        }
        sum += s;
        if (k == 0) id = byte_at(v, 15) | (byte_at(v, 16) << 8);
      }
      r.sum = sum; r.id = id;
    } else {
      r.sum = 0xEEEE0000u | (unsigned)m.op_own;
    }
    out.write(r);
  }
}

static void writer(hls::stream<rsl_t> &in, ap_uint<64> *res, volatile uint32_t *dbw, unsigned cq_dbr_off,
                   unsigned rq_dbr_off, unsigned log_rq)
{
  const unsigned rq_size = 1u << log_rq;
  unsigned got = 0, errs = 0, last = 0;
wr:
  for (;;) {
    const rsl_t r = in.read();
    if (r.end) {
      dbw[cq_dbr_off / 4] = bswap32(got & 0xffffff);
      dbw[rq_dbr_off / 4] = bswap32((got + rq_size) & 0xffff);
      res[0] = ((ap_uint<64>)r.polls << 32) | got;
      res[1] = ((ap_uint<64>)last << 32) | errs;
      break;
    }
    res[2 + got] = ((ap_uint<64>)r.id << 32) | r.sum;
    if (r.err) errs++; else last = r.len;
    got++;
    if ((got & 15) == 0) {                               // batch the doorbell records
      dbw[cq_dbr_off / 4] = bswap32(got & 0xffffff);
      dbw[rq_dbr_off / 4] = bswap32((got + rq_size) & 0xffff);
    }
  }
}

extern "C" void e2_rx3(const beat_t *cqp, volatile const uint32_t *ctl, const beat_t *data, ap_uint<64> *res,
                       volatile uint32_t *dbw, unsigned cq_off, unsigned log_cq, unsigned cq_dbr_off,
                       unsigned rq_dbr_off, unsigned log_rq, unsigned data_off, unsigned stride, unsigned n,
                       unsigned max_polls, unsigned stop_off)
{
#pragma HLS INTERFACE m_axi port=cqp  offset=slave bundle=gmem0 depth=131072 max_read_burst_length=4 num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=ctl  offset=slave bundle=gmem1 depth=1048576 max_read_burst_length=2
#pragma HLS INTERFACE m_axi port=data offset=slave bundle=gmem2 depth=131072 max_read_burst_length=16 num_read_outstanding=32
#pragma HLS INTERFACE m_axi port=res  offset=slave bundle=gmem3 depth=65538 max_write_burst_length=2 num_write_outstanding=32
#pragma HLS INTERFACE m_axi port=dbw  offset=slave bundle=gmem4 depth=1048576 max_write_burst_length=2
#pragma HLS DATAFLOW
  hls::stream<cmd_t, 256> cmds;
  hls::stream<rsl_t, 256> rsls;
  poller(cqp, ctl, cmds, cq_off, log_cq, n, max_polls, stop_off);
  checksum(data, cmds, rsls, data_off, stride, log_rq);
  writer(rsls, res, dbw, cq_dbr_off, rq_dbr_off, log_rq);
}
