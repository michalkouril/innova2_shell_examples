// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.2 adapter between the FLD-lite engines and the FlexDriver ZUC AFU (2026-09-30). Two free-running kernels:
//
//   fld_rx --pkt_t--> zuc_in --afu_t--> zuc_afu (RTL) --afu_t--> zuc_out --pkt_t--> fld_tx
//
// Our streams (pkt_t, as fld_lite.cpp): byte k of the frame at data bits [8k+7:8k], keep bit k = byte k, user[15:0] =
// frame length on every beat. The AFU's streams (afu_t): MSB-first, byte 0 at tdata[511:504], tkeep MSB-aligned,
// 72-bit tuser. Protections required by the AFU (work/docs/zuc_afu_port_spec.md 0):
//   zuc_in  stores each request and forwards ONLY well-formed ones: opcode 0 (CONF) or 1 (INTEG), 1 <= len <= 8192
//           (8191 for INTEG: a 16-bit bit counter wraps), exactly 2 + ceil(len/64) beats. A runt or a zero-length
//           message corrupts the AFU permanently; a malformed one is echoed back with key and IV in clear.
//           tuser is held CONSTANT at 0 (the AFU samples the reply's tuser from the bus after the request).
//   zuc_out stores each response (fld_tx needs the length on the first beat) and DROPS any whose IP checksum
//           (frame bytes 24-25) is non-zero: processed replies always carry 0, bypass echoes keep the original.
// Header (frame bytes 64..): [64] opcode, [65] reserved, [66..67] length BE, [68..75] message id, [76..91] key,
// [92..107] IV. Ethernet mode: frame bytes 0..63 are an L2-L4 header the AFU swaps, zeroing both checksums.
#include <ap_int.h>
#include <ap_axi_sdata.h>
#include <hls_stream.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;
typedef ap_axiu<512, 72, 0, 0> afu_t;

static beat_t rev_bytes(const beat_t &d)
{
  beat_t r;
  for (int k = 0; k < 64; k++) {
#pragma HLS UNROLL
    r.range(8 * (63 - k) + 7, 8 * (63 - k)) = d.range(8 * k + 7, 8 * k);
  }
  return r;
}
static unsigned popcount64(const ap_uint<64> &k)
{
  unsigned c = 0;
  for (int i = 0; i < 64; i++) {
#pragma HLS UNROLL
    c += k[i];
  }
  return c;
}
static ap_uint<64> rev_bits(const ap_uint<64> &k)
{
  ap_uint<64> r;
  for (int i = 0; i < 64; i++) {
#pragma HLS UNROLL
    r[63 - i] = k[i];
  }
  return r;
}

extern "C" void zuc_in(hls::stream<pkt_t> &in, hls::stream<afu_t> &out)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
  beat_t buf[256];                                                  // one request: <= 2 + 128 beats (8 KB payload)
  ap_uint<64> keep[256];
lp:
  while (1) {
    unsigned n = 0;
    bool last = false;
  rd:
    while (!last) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=130
      const pkt_t p = in.read();
      buf[n & 255] = p.data; keep[n & 255] = p.keep;
      last = p.last;
      n++;
    }
    const beat_t h = buf[1];
    const unsigned op = (unsigned)h.range(7, 0), len = ((unsigned)h.range(23, 16) << 8) | (unsigned)h.range(31, 24);
    const unsigned need = 2 + ((len + 63) >> 6);
    const bool ok = n >= 3 && n <= 130 && (op == 0 || op == 1) && len >= 1 && len <= (op == 0 ? 8192u : 8191u) && n == need;
    if (!ok) continue;                                              // drop: never let it reach the AFU
  wr:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=130
      afu_t a;
      a.data = rev_bytes(buf[i]);
      a.keep = rev_bits(keep[i]);
      a.strb = a.keep;
      a.last = i == n - 1;
      a.user = 0;                                                   // constant: chid 0, Ethernet, EOM forced by CTRL0[28]
      out.write(a);
    }
  }
}

extern "C" void zuc_out(hls::stream<afu_t> &in, hls::stream<pkt_t> &out)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
  beat_t buf[256];                                                  // one response: 9 KB + header < 256 beats
  ap_uint<64> lastkeep = 0;
lp:
  while (1) {
    unsigned n = 0, len = 0;
    bool last = false, echo = false;
  rd:
    while (!last) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=160
      const afu_t a = in.read();
      const ap_uint<64> k = rev_bits(a.keep);
      const beat_t d = rev_bytes(a.data);
      buf[n & 255] = d;
      if (n == 0) echo = d.range(8 * 25 + 7, 8 * 24) != 0;          // IP checksum != 0: a bypass echo
      last = a.last;
      if (last) { lastkeep = k; len = 64 * n + popcount64(k); }
      n++;
    }
    if (echo) continue;                                             // drop it (it carries key and IV in clear)
  wr:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=160
      pkt_t p;
      p.data = buf[i & 255];
      p.keep = i == n - 1 ? lastkeep : ap_uint<64>(-1);
      p.strb = p.keep;
      p.user = len & 0xffff;
      p.last = i == n - 1;
      out.write(p);
    }
  }
}
