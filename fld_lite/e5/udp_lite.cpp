// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// UDP architecture step 2 (work/docs/udp_stack_architecture.md): the thin FPGA side of the split UDP stack, under the
// application interface of ETH Zurich's fpga-network-stack (fns: ipUdpMeta + 512-bit data streams, byte k of the
// stream at data[8k+7:8k]). All kernels are free-running (ap_ctrl_none) stream kernels between fld_rx and fld_tx:
//
//   fld_rx --pkt_t--> udp_rx_lite --meta/data--> application (udp_echo_app) --meta/data--> udp_tx_lite --pkt_t--> fld_tx
//                          \------------------------------ learn ------------------------------/
//
// The NIC has already done the rest (steering on {IPv4, UDP, dst ip, dst port, !frag}, IP/UDP checksum checks reported
// in the CQE and enforced by fld_rx pre bit 17, padding of short frames, IP/UDP checksum generation via fld_tx cs_flags);
// Linux answers ARP / ICMP / closed ports. So:
//   udp_rx_lite  fixed offsets (IPv4, IHL 5 -- fld_rx drops IP options), meta = fns ipUdpMeta, payload realigned by the
//                42-byte header and trimmed to the UDP length (drops Ethernet padding); sends a learn record
//                {their ip, their mac, my ip, my mac} per datagram to udp_tx_lite.
//   udp_tx_lite  builds Ethernet + IPv4 + UDP (checksums 0 -> the NIC), destination MAC from a 64-entry table learned
//                from received datagrams (direct-mapped on the low 6 bits of the address); a miss drops the datagram.
//                Source MAC/IP = the ones the NIC steered to us (learned too), so no configuration registers.
// fns ipUdpMeta conventions: their_address bits 127:96 = IPv4 address (here: the four wire bytes, byte 26 of the frame
// in bits 103:96, i.e. as fns stores it, "network order in little-endian lanes"), ports and length in host order; on
// RX length = UDP length (payload + 8), on TX length = payload bytes.
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>

typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;          // FLD-lite frames: user[15:0] = frame length on every beat
typedef ap_axiu<512, 0, 0, 0> nax_t;           // fns net_axis<512>: data / keep / last
typedef ap_uint<176> meta_t;                   // fns ipUdpMeta packed: [127:0] their_address, [143:128] their_port,
                                               // [159:144] my_port, [175:160] length
typedef ap_uint<160> learn_t;                  // [31:0] their ip (wire bytes), [79:32] their mac, [111:80] my ip,
                                               // [159:112] my mac (wire byte order, byte 0 in the low bits)

static inline ap_uint<8> g8(const beat_t &d, int i) { return d.range(8 * i + 7, 8 * i); }
static inline void p8(beat_t &d, int i, ap_uint<8> v) { d.range(8 * i + 7, 8 * i) = v; }
static inline ap_uint<64> keep_n(unsigned n) { return n >= 64 ? ap_uint<64>(-1) : ap_uint<64>((ap_uint<65>(1) << n) - 1); }

static inline meta_t mk_meta(ap_uint<32> ip, ap_uint<16> their_port, ap_uint<16> my_port, ap_uint<16> len)
{
  meta_t m = 0; m.range(127, 96) = ip; m.range(143, 128) = their_port; m.range(159, 144) = my_port; m.range(175, 160) = len;
  return m;
}

// ------------------------------------------------------------------------------------------------ RX
// one frame (all its beats); emits meta + payload beats for a good IPv4/UDP datagram, consumes and drops anything else
void udp_rx_step(hls::stream<pkt_t> &in, hls::stream<meta_t> &meta, hls::stream<nax_t> &data, hls::stream<learn_t> &learn)
{
#pragma HLS INLINE
  pkt_t p = in.read();
  const beat_t b0 = p.data;
  const unsigned flen = p.user & 0xffff;
  const ap_uint<16> et = (g8(b0, 12), g8(b0, 13));
  const ap_uint<16> ulen = (g8(b0, 38), g8(b0, 39));
  const bool ok = et == 0x0800 && g8(b0, 14) == 0x45 && g8(b0, 23) == 17 && ulen >= 8 && unsigned(ulen) + 34 <= flen;
  const unsigned plen = ok ? unsigned(ulen) - 8 : 0;
  if (ok) {
    // learn first: the reply's meta must never reach udp_tx_lite before the addresses it needs
    learn_t l = 0;
    l.range(31, 0) = b0.range(29 * 8 + 7, 26 * 8);   // their ip (frame src ip)
    l.range(79, 32) = b0.range(11 * 8 + 7, 6 * 8);    // their mac (frame src mac)
    l.range(111, 80) = b0.range(33 * 8 + 7, 30 * 8);  // my ip (frame dst ip)
    l.range(159, 112) = b0.range(5 * 8 + 7, 0);       // my mac (frame dst mac)
    learn.write(l);
    meta.write(mk_meta(b0.range(29 * 8 + 7, 26 * 8), (g8(b0, 34), g8(b0, 35)), (g8(b0, 36), g8(b0, 37)), ulen));
  }
  // payload realignment: out beat k = in beat k bytes 42..63 (-> 0..21) + in beat k+1 bytes 0..41 (-> 22..63)
  beat_t prev = b0;
  unsigned sent = 0;
  bool last = p.last;
  // a 1..22-byte payload fits in beat 0 alone
  if (ok && plen != 0 && plen <= 22) {
    nax_t o; o.data = prev >> (42 * 8); o.keep = keep_n(plen); o.last = 1; data.write(o); sent = plen;
  }
rx:
  while (!last) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=24
    p = in.read();
    last = p.last;
    if (ok && sent < plen) {
      nax_t o;
      o.data = (prev >> (42 * 8)) | (p.data << (22 * 8));
      const unsigned left = plen - sent;
      o.keep = keep_n(left);
      o.last = left <= 64;
      data.write(o);
      sent += left <= 64 ? left : 64;
    }
    prev = p.data;
  }
  // payload ending inside the last input beat's bytes 42..63 (only possible when the frame had no further beat)
  if (ok && sent < plen) {
    nax_t o; o.data = prev >> (42 * 8); o.keep = keep_n(plen - sent); o.last = 1; data.write(o);
  }
}

extern "C" void udp_rx_lite(hls::stream<pkt_t> &in, hls::stream<meta_t> &meta, hls::stream<nax_t> &data, hls::stream<learn_t> &learn)
{
#pragma HLS INTERFACE axis port=in
#pragma HLS INTERFACE axis port=meta
#pragma HLS INTERFACE axis port=data
#pragma HLS INTERFACE axis port=learn
#pragma HLS INTERFACE ap_ctrl_none port=return
lp:
  while (1) udp_rx_step(in, meta, data, learn);
}

// ------------------------------------------------------------------------------------------------ application
// UDP echo on the fns interface: reply to the sender from the port it addressed, same payload
void udp_echo_step(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<meta_t> &txm, hls::stream<nax_t> &txd)
{
#pragma HLS INLINE
  const meta_t m = rxm.read();
  const ap_uint<16> len = m.range(175, 160);                     // RX: UDP length = payload + 8
  txm.write(mk_meta(m.range(127, 96), m.range(143, 128), m.range(159, 144), len - 8));
  if (len > 8) {
    nax_t d;
  cp:
    do {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=23
      d = rxd.read(); txd.write(d);
    } while (!d.last);
  }
}

extern "C" void udp_echo_app(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<meta_t> &txm, hls::stream<nax_t> &txd)
{
#pragma HLS INTERFACE axis port=rxm
#pragma HLS INTERFACE axis port=rxd
#pragma HLS INTERFACE axis port=txm
#pragma HLS INTERFACE axis port=txd
#pragma HLS INTERFACE ap_ctrl_none port=return
lp:
  while (1) udp_echo_step(rxm, rxd, txm, txd);
}

// ------------------------------------------------------------------------------------------------ TX
struct tx_state {
  ap_uint<32> tip[64];
  ap_uint<48> tmac[64];
  bool tval[64];
  ap_uint<32> my_ip;
  ap_uint<48> my_mac;
  bool have_self;
  ap_uint<16> ip_id;
};

// apply every pending learn record, then send one datagram if one is waiting
void udp_tx_step(hls::stream<meta_t> &txm, hls::stream<nax_t> &txd, hls::stream<learn_t> &learn, hls::stream<pkt_t> &out, tx_state &st)
{
#pragma HLS INLINE
  learn_t l;
lr:
  while (learn.read_nb(l)) {
#pragma HLS PIPELINE II=1
    const unsigned i = l.range(31, 24) & 63;                      // last wire byte of the address (x.y.z.W): its low 6 bits
    st.tip[i] = l.range(31, 0); st.tmac[i] = l.range(79, 32); st.tval[i] = true;
    st.my_ip = l.range(111, 80); st.my_mac = l.range(159, 112); st.have_self = true;
  }
  meta_t m;
  if (!txm.read_nb(m)) return;
  const ap_uint<32> dip = m.range(127, 96);
  const ap_uint<16> dport = m.range(143, 128), sport = m.range(159, 144), plen = m.range(175, 160);
  const unsigned i = dip.range(31, 24) & 63;
  const bool hit = st.have_self && st.tval[i] && st.tip[i] == dip;
  const unsigned flen = 42 + unsigned(plen);
  const ap_uint<16> iplen = 28 + plen, ulen = 8 + plen;
  beat_t h = 0;
  h.range(5 * 8 + 7, 0) = st.tmac[i];                              // dst mac
  h.range(11 * 8 + 7, 6 * 8) = st.my_mac;                          // src mac
  p8(h, 12, 0x08); p8(h, 13, 0x00);
  p8(h, 14, 0x45); p8(h, 15, 0x00); p8(h, 16, iplen >> 8); p8(h, 17, iplen & 0xff);
  p8(h, 18, st.ip_id >> 8); p8(h, 19, st.ip_id & 0xff); p8(h, 20, 0x40); p8(h, 21, 0x00);   // DF
  p8(h, 22, 64); p8(h, 23, 17);                                     // TTL, UDP; header checksum 24-25 = 0 (NIC)
  h.range(29 * 8 + 7, 26 * 8) = st.my_ip;
  h.range(33 * 8 + 7, 30 * 8) = dip;
  p8(h, 34, sport >> 8); p8(h, 35, sport & 0xff); p8(h, 36, dport >> 8); p8(h, 37, dport & 0xff);
  p8(h, 38, ulen >> 8); p8(h, 39, ulen & 0xff);                    // UDP checksum 40-41 = 0 (NIC)
  if (hit) st.ip_id++;
  // payload: out beat 0 = header (0..41) + payload 0..21; out beat k = in beat k-1 bytes 22..63 + in beat k bytes 0..21
  beat_t cur = 0, prev = 0;
  bool more = plen != 0, first = true;
  unsigned outb = 0;
  const unsigned nout = (flen + 63) / 64;
  if (plen != 0) { nax_t d = txd.read(); cur = d.data; more = !d.last; }
tx:
  for (unsigned k = 0; k < nout || more; k++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=24
    beat_t o;
    if (k == 0) o = h | (cur << (42 * 8));
    else o = (prev >> (22 * 8)) | (cur << (42 * 8));
    if (k < nout && hit) {
      pkt_t q; q.data = o; q.user = flen;
      const unsigned rem = flen - 64 * k;
      q.keep = keep_n(rem); q.strb = q.keep; q.last = k == nout - 1;
      out.write(q);
    }
    prev = cur;
    if (more) { nax_t d = txd.read(); cur = d.data; more = !d.last; } else cur = 0;
  }
}

extern "C" void udp_tx_lite(hls::stream<meta_t> &txm, hls::stream<nax_t> &txd, hls::stream<learn_t> &learn, hls::stream<pkt_t> &out)
{
#pragma HLS INTERFACE axis port=txm
#pragma HLS INTERFACE axis port=txd
#pragma HLS INTERFACE axis port=learn
#pragma HLS INTERFACE axis port=out
#pragma HLS INTERFACE ap_ctrl_none port=return
  static tx_state st;
  for (int i = 0; i < 64; i++) st.tval[i] = false;
  st.have_self = false; st.ip_id = 0;
lp:
  while (1) udp_tx_step(txm, txd, learn, out, st);
}
