// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// iperf_fns -- ETH Zurich fpga-network-stack's iperf_udp client (BSD-3-Clause; hls/iperf_udp/iperf_udp.cpp of the
// commit ../fetch_fns.sh checks out) running UNCHANGED on top of the UDP layer (udp_rx_lite / udp_tx_lite), as a
// Vitis kernel. fns drives iperf_udp's four controls (runExperiment, pkgWordCount, packetGap, targetIpAddress) as
// ap_none wires from RTL registers, which a Vitis kernel cannot have; this wrapper calls fns's own client() / clock() /
// check_for_response() once per cycle in a free-running loop -- what fns's DATAFLOW top does -- converts between our
// streams (meta_t / nax_t, udp_lite.cpp) and fns's (ipUdpMeta / net_axis<512>), and takes the controls from a small
// host-started kernel, iperf_ctl, over a stream.
// fns's clock() runs in its OWN free-running kernel, iperf_clock, linked to the client by two 8-bit streams (start,
// stop) -- as in fns, where clock is a separate DATAFLOW process. In v1 it shared the client's loop, and back-pressure
// from udp_tx_lite stalled that loop: the "5 s" experiment ran 40-55 s on hardware (2026-10-03). The timer now counts
// iperf_clock's own cycles: END_TIME_5s = 781.25 M cycles = 7.8 s at 100 MHz, 3.1 s at 250 MHz.
//   cfg word: [0] runExperiment, [15:8] pkgWordCount (64-byte words per datagram), [23:16] packetGap (cycles),
//             [63:32] targetIpAddress (wire byte order: first octet in the low byte, as fns and udp_lite store it)
// fns quirks kept as they are: the experiment length (END_TIME_5s) is counted in 156.25 MHz cycles, i.e. ~2.6 s at
// 300 MHz; datagrams carry pkgWordCount * 64 payload bytes to port 5001 from port 32768 (fns MY_PORT); the last one
// has a negative sequence number and the client waits up to 100 ms (fns time) for the iperf2 server report.
// fns builds ipUdpMeta from a 32-bit address (it lands in their_address[31:0]) while its IPv4 code reads [127:96];
// the conversion below accepts either.
#define FNS_DATA_WIDTH 64
#define FNS_ROCE_STACK_MAX_QPS 500
#include "iperf_udp/iperf_udp.cpp"
#include <ap_axi_sdata.h>

typedef ap_axiu<512, 0, 0, 0> nax_t;
typedef ap_uint<176> meta_t;

static inline meta_t to_ours(const ipUdpMeta &m)
{
  meta_t o = 0;
  const ap_uint<32> hi = m.their_address(127, 96), lo = m.their_address(31, 0);
  o.range(127, 96) = hi != 0 ? hi : lo;
  o.range(143, 128) = m.their_port; o.range(159, 144) = m.my_port; o.range(175, 160) = m.length;
  return o;
}

struct iperf_state {
  hls::stream<ipUdpMeta> f_rxm, f_txm;
  hls::stream<net_axis<512> > f_rxd, f_txd;
  hls::stream<bool> startS, stopS, doneS;
  ap_uint<1> run; ap_uint<8> wc, gap; ap_uint<32> ip;
};

typedef ap_uint<8> sig_t;

// one cycle of the free-running loop (called directly by the csim test)
void iperf_step(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<meta_t> &txm, hls::stream<nax_t> &txd,
                hls::stream<ap_uint<64> > &cfg, hls::stream<sig_t> &start_o, hls::stream<sig_t> &stop_i, iperf_state &s)
{
#pragma HLS INLINE
  ap_uint<64> c;
  if (cfg.read_nb(c)) { s.run = c[0]; s.wc = c.range(15, 8); s.gap = c.range(23, 16); s.ip = c.range(63, 32); }
  // forward to the outputs only when they have room, and call client() (at most one meta + one data word per call)
  // only when its FIFOs have room too
  if (!txm.full() && !s.f_txm.empty()) txm.write(to_ours(s.f_txm.read()));
  if (!txd.full() && !s.f_txd.empty()) { net_axis<512> td = s.f_txd.read(); nax_t o; o.data = td.data; o.keep = td.keep; o.last = td.last; txd.write(o); }
  meta_t m; if (!s.f_rxm.full() && rxm.read_nb(m)) s.f_rxm.write(ipUdpMeta(m.range(127, 0), m.range(143, 128), m.range(159, 144), m.range(175, 160)));
  nax_t d; if (!s.f_rxd.full() && rxd.read_nb(d)) s.f_rxd.write(net_axis<512>(d.data, d.keep, d.last));
  if (!s.f_txm.full() && !s.f_txd.full())
    client<512>(s.f_txm, s.f_txd, s.startS, s.stopS, s.doneS, s.run, s.wc, s.gap, s.ip);
  bool b; sig_t g;
  if (!start_o.full() && s.startS.read_nb(b)) start_o.write(b);
  if (!s.stopS.full() && stop_i.read_nb(g)) s.stopS.write(g != 0);
  check_for_response<512>(s.f_rxm, s.f_rxd, s.doneS);
}

extern "C" void iperf_udp_fns(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<meta_t> &txm,
                              hls::stream<nax_t> &txd, hls::stream<ap_uint<64> > &cfg, hls::stream<sig_t> &start_o,
                              hls::stream<sig_t> &stop_i)
{
#pragma HLS INTERFACE axis port=rxm
#pragma HLS INTERFACE axis port=rxd
#pragma HLS INTERFACE axis port=txm
#pragma HLS INTERFACE axis port=txd
#pragma HLS INTERFACE axis port=cfg
#pragma HLS INTERFACE axis port=start_o
#pragma HLS INTERFACE axis port=stop_i
#pragma HLS INTERFACE ap_ctrl_none port=return
  static iperf_state s;
  s.run = 0; s.wc = 22; s.gap = 0; s.ip = 0;
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    iperf_step(rxm, rxd, txm, txd, cfg, start_o, stop_i, s);
  }
}

// fns clock(), unchanged, one call per cycle of its own (csim calls iperf_clock_step directly)
struct clock_state { hls::stream<bool> st, sp; };
void iperf_clock_step(hls::stream<sig_t> &start_i, hls::stream<sig_t> &stop_o, clock_state &c)
{
#pragma HLS INLINE
  sig_t g; bool b;
  if (!c.st.full() && start_i.read_nb(g)) c.st.write(g != 0);
  clock(c.st, c.sp);
  if (!stop_o.full() && c.sp.read_nb(b)) stop_o.write(b);
}

extern "C" void iperf_clock(hls::stream<sig_t> &start_i, hls::stream<sig_t> &stop_o)
{
#pragma HLS INTERFACE axis port=start_i
#pragma HLS INTERFACE axis port=stop_o
#pragma HLS INTERFACE ap_ctrl_none port=return
  static clock_state c;
#pragma HLS STREAM variable=c.st depth=4
#pragma HLS STREAM variable=c.sp depth=4
lp:
  while (1) {
#pragma HLS PIPELINE II=1
    iperf_clock_step(start_i, stop_o, c);
  }
}

// host-started: one control word for iperf_udp_fns, then return
extern "C" void iperf_ctl(hls::stream<ap_uint<64> > &cfg, unsigned run, unsigned words, unsigned gap, unsigned ip)
{
#pragma HLS INTERFACE axis port=cfg
#pragma HLS INTERFACE s_axilite port=run
#pragma HLS INTERFACE s_axilite port=words
#pragma HLS INTERFACE s_axilite port=gap
#pragma HLS INTERFACE s_axilite port=ip
#pragma HLS INTERFACE s_axilite port=return
  ap_uint<64> c = 0;
  c[0] = run & 1; c.range(15, 8) = words; c.range(23, 16) = gap; c.range(63, 32) = ip;
  cfg.write(c);
}
