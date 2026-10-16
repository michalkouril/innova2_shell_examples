// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// h4ml_jet -- an hls4ml neural network inline in FLD-lite: a UDP endpoint that classifies every datagram. Sits where
// udp_swap sits (fld_rx -> AFU -> fld_tx, free-running) and, like udp_swap, turns each frame into its echo in place
// (swap MAC / IP / port, zero the IP and UDP checksums: fld_tx sets cs_flags L3|L4 and the NIC recomputes them). A frame
// whose payload starts with "JET1" carries 16 jet features; the kernel runs hls4ml's jet-tagging network on them
// (firmware/: 16 -> 64 -> 32 -> 32 -> 5, ReLU, ap_fixed<16,6>, fully parallel) and writes the 5 class scores and the
// winning class into the reply. Payload layout (offset from the UDP payload, frame offset in brackets):
//   0..3  "JET1" magic [42..45]        4..7  sequence number, echoed [46..49]
//   8..39 16 x int16 LE: the features as raw ap_fixed<16,6> (value * 1024) [50..81: beat 0 50..63, beat 1 0..17]
//   40..49 5 x int16 LE: the class scores, raw ap_fixed<16,6> (written) [beat 1 18..27]
//   50    the winning class 0..4 (written) [beat 1 28]      51  status: 1 = classified (written) [beat 1 29]
// Frames that are shorter than two beats or lack the magic are echoed unchanged apart from the header swap.
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>
#include "firmware/jetnet.h"

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;

static inline ap_uint<8> g8(const beat_t &d, int i) { return d.range(8 * i + 7, 8 * i); }
static inline void p8(beat_t &d, int i, ap_uint<8> v) { d.range(8 * i + 7, 8 * i) = v; }

static void swap_hdr(beat_t &d)
{
#pragma HLS INLINE
  const beat_t s = d;
  for (int i = 0; i < 6; i++) { p8(d, i, g8(s, 6 + i)); p8(d, 6 + i, g8(s, i)); }            // MACs
  for (int i = 0; i < 4; i++) { p8(d, 26 + i, g8(s, 30 + i)); p8(d, 30 + i, g8(s, 26 + i)); }  // IPs
  for (int i = 0; i < 2; i++) { p8(d, 34 + i, g8(s, 36 + i)); p8(d, 36 + i, g8(s, 34 + i)); }  // ports
  p8(d, 24, 0); p8(d, 25, 0); p8(d, 40, 0); p8(d, 41, 0);                                 // checksums -> NIC
}

// byte k of the 128-byte window formed by beats 0 and 1
static inline ap_uint<8> wb(const beat_t &b0, const beat_t &b1, int k) { return k < 64 ? g8(b0, k) : g8(b1, k - 64); }

// one frame (called by the free-running top and directly by the C testbench)
void h4ml_frame(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out)
{
#pragma HLS INLINE off
  pkt_t p0 = in.read();
  beat_t b0 = p0.data;
  swap_hdr(b0);
  p0.data = b0;
  if (p0.last) { out.write(p0); return; }
  pkt_t p1 = in.read();
  beat_t b1 = p1.data;
  const bool magic = g8(b0, 42) == 'J' && g8(b0, 43) == 'E' && g8(b0, 44) == 'T' && g8(b0, 45) == '1';
  if (magic) {
    input_t x[N_INPUT_1_1];
    result_t y[N_LAYER_8];
#pragma HLS ARRAY_PARTITION variable=x complete
#pragma HLS ARRAY_PARTITION variable=y complete
    for (int i = 0; i < N_INPUT_1_1; i++) {
#pragma HLS UNROLL
      ap_uint<16> r = (ap_uint<16>(wb(b0, b1, 51 + 2 * i)) << 8) | wb(b0, b1, 50 + 2 * i);
      x[i].range(15, 0) = r;
    }
    jetnet(x, y);
    int best = 0;
    for (int c = 1; c < N_LAYER_8; c++) {
#pragma HLS UNROLL
      if (y[c] > y[best]) best = c;
    }
    for (int c = 0; c < N_LAYER_8; c++) {
#pragma HLS UNROLL
      ap_uint<16> r = y[c].range(15, 0);
      p8(b1, 18 + 2 * c, r.range(7, 0)); p8(b1, 19 + 2 * c, r.range(15, 8));
    }
    p8(b1, 28, best); p8(b1, 29, 1);
    p1.data = b1;
  }
  out.write(p0);
  out.write(p1);
  bool last = p1.last;
rest:
  while (!last) {
#pragma HLS PIPELINE II=1
    pkt_t p = in.read();
    out.write(p);
    last = p.last;
  }
}

extern "C" void h4ml_jet(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
lp:
  while (1) h4ml_frame(in, out);
}
