// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// udp_swap -- UDP architecture step 1 (work/docs/udp_stack_architecture.md): the smallest FPGA UDP endpoint. Sits where
// afu_loop sits (fld_rx -> AFU -> fld_tx, free-running) and turns every received IPv4/UDP frame into its echo IN PLACE:
// swap Ethernet src/dst MAC, IPv4 src/dst address and UDP src/dst port, zero the IP header and UDP checksums (fld_tx sets
// cs_flags L3|L4, so the NIC recomputes both). Payload untouched, no realignment, no tables. Frames arrive already
// filtered by the NIC (steering on {IPv4, UDP, dst ip, dst port, !frag}; fld_rx pre bit 17 drops bad checksums and IP
// options), so fixed offsets hold: Ethernet 0-13, IPv4 14-33 (csum 24-25, src 26-29, dst 30-33), UDP 34-41
// (sport 34-35, dport 36-37, csum 40-41). All in beat 0 (64 bytes).
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;

static inline ap_uint<8> g8(const beat_t &d, int i) { return d.range(8 * i + 7, 8 * i); }
static inline void p8(beat_t &d, int i, ap_uint<8> v) { d.range(8 * i + 7, 8 * i) = v; }

// one beat (inlined into the free-running loop; called directly by the csim test)
void udp_swap_step(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, bool &first)
{
#pragma HLS INLINE
  pkt_t p = in.read();
  if (first) {
    const beat_t s = p.data; beat_t d = s;
    for (int i = 0; i < 6; i++) { p8(d, i, g8(s, 6 + i)); p8(d, 6 + i, g8(s, i)); }            // MACs
    for (int i = 0; i < 4; i++) { p8(d, 26 + i, g8(s, 30 + i)); p8(d, 30 + i, g8(s, 26 + i)); }  // IPs
    for (int i = 0; i < 2; i++) { p8(d, 34 + i, g8(s, 36 + i)); p8(d, 36 + i, g8(s, 34 + i)); }  // ports
    p8(d, 24, 0); p8(d, 25, 0); p8(d, 40, 0); p8(d, 41, 0);                                 // checksums -> NIC
    p.data = d;
  }
  first = p.last;
  out.write(p);
}

extern "C" void udp_swap(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
  bool first = true;
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    udp_swap_step(in, out, first);
  }
}
