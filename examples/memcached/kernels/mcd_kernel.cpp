// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// mcd -- the Xilinx HLx memcached pipeline (BSD-3-Clause, work/ext/HLx_Examples/Acceleration/memcached, 2015; built
// from ../fix1 = the sources with the remux/flashRemux fall-through fix) as one free-running Vitis kernel on the UDP
// step-2 stack (udp_rx_lite -> mcd -> udp_tx_lite, fns ipUdpMeta-style API) with its memories in DDR.
//
//   rx_adapt   meta_t + 512-bit datagram beats -> the pipeline's 64-bit extendedAxiWord (user = client port / IP,
//              local port, as the original udpShim packs them; the pipeline only swaps them back)
//   the 7 stages of memcachedPipeline(), unchanged (binary protocol only; payload = the binary header directly,
//              NO 8-byte memcached UDP frame header -- clients must send raw binary requests)
//   tx_adapt   64-bit response words -> meta_t (length = user[111:96]) + 512-bit beats
//   mem_svc x2 hash table / DRAM value store: memCtrlWord {line address, line count} commands + 512-bit data ->
//              m_axi bursts at ht / vs; one command at a time, writes first (as the reference DRAM model), so a
//              read never overtakes an earlier write
//   alloc      the value-store free list (the original asked the host over PCIe): fresh 2 KB slots 0..VS_SLOTS-1,
//              then recycled ones; 2 KB = max DRAM value 2047 B + 4 B flags rounded to 64 B lines (1 KB slots
//              overflow: csim ALLSEQS showed it)
//   flash_stub the flash value store is not built: no flash addresses are ever handed out, so values >= 2048 B
//              fail their SET cleanly; any flash command is drained (reads answered with zeros)
// v2 (2026-10-04): the memory servers are a SEPARATE host-started kernel, mcd_mem (ht / vs as normal kernel
// arguments). In v1 they sat inside the free-running mcd with AXI-Lite pointer registers: the hash table zeroes
// itself right after reset, BEFORE the host had written the pointers, and those writes landed relative to address 0
// -- on FLD-lite's rings (hpy440, 2026-10-04 02:5x: no replies, garbage RX CQEs). Now mcd has no memory port at all;
// its commands wait in the streams (back-pressure) until the host starts mcd_mem with the right buffers.
// v3 (2026-10-04): every process of OURS inside the mcd dataflow region is strictly non-blocking (writes only when the
// stream has room, reads with read_nb). On hpy440 the core froze after 13 start-up hash-table writes: alloc_assign_proc
// blocked writing free slots into the 16-deep addrDram stream (nothing consumes them before a SET), and a blocked
// process stalls the whole region. C simulation cannot show it (unbounded streams). Writes to the OTHER kernels'
// streams (txm/txd) may block: those consumers drain independently.
// Flush (binary FLUSH) is acknowledged at once (flush_ack tied 1): the hash table is cleared, the slot allocator is
// NOT reset (slots of flushed keys leak until the kernel is reloaded) -- a later fix.
// Hash table size is the demo's: 7 bits of hash x 8 lines (64 KB, ~512 keys). Scaling it is a later step.
#include "globals.h"
#include <ap_axi_sdata.h>

typedef ap_axiu<512, 0, 0, 0> nax_t;
typedef ap_uint<176> meta_t;    // [127:96] their IPv4, [143:128] their port, [159:144] my port, [175:160] length

// debug counters (2026-10-04): tagged snapshots [63:56] id, [55:0] count, sent non-blocking every 64 K calls to the
// host-started mcd_dbg kernel, which stores them at out[id]; a stalled stage simply stops advancing.
//   1 rx beats in   2 rx words to the pipeline   3 tx words from the pipeline   4 tx responses   5-8 ht rd / ht wr /
//   vs rd / vs wr commands issued   16-23 mcd_mem: ht rd cmds, ht rd words, ht wr cmds, ht wr words, vs (same 4)
//   24 mcd_mem loop iterations
typedef ap_uint<64> dbg_t;
#ifndef DBG_TICK_BITS
#define DBG_TICK_BITS 16   // snapshot every 2^16 calls (csim tests use fewer)
#endif
typedef ap_uint<DBG_TICK_BITS> dbg_tick_t;
static inline dbg_t dbg_word(unsigned id, ap_uint<56> v) { dbg_t w = 0; w.range(63, 56) = id; w.range(55, 0) = v; return w; }
static const unsigned SLOT_LINES = 32;          // 2 KB value-store slots
static const unsigned VS_SLOTS = 32768;         // 64 MB value store
static const unsigned RET_DEPTH = 1024;         // recycled-slot FIFO

// ---------------------------------------------------------------------------------------------------------------
// RX: one 512-bit beat per call (with the datagram's metadata at its start), its 64-bit words emitted by a pipelined
// loop -- a whole beat per call keeps the pipeline input gap-free in C simulation too
void rx_proc(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<extendedAxiWord> &toPipe, hls::stream<dbg_t> &dbg)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static bool inpkt = false, have = false;
  static ap_uint<112> user = 0;
  static nax_t beat;
  static ap_uint<4> k = 0, n = 1;
  static ap_uint<56> nbeat = 0, nword = 0;
  static dbg_tick_t tick = 0;
  if (++tick == 0) { dbg.write_nb(dbg_word(1, nbeat)); dbg.write_nb(dbg_word(2, nword)); }
  // take a new beat (and, at a datagram start, its metadata) when none is held
  if (!have && !rxd.empty() && (inpkt || !rxm.empty())) {
    if (!inpkt) {
      const meta_t m = rxm.read();
      ap_uint<112> u = 0;
      u.range(15, 0) = m.range(143, 128);     // client (their) port
      u.range(47, 16) = m.range(127, 96);     // client IPv4
      u.range(63, 48) = m.range(159, 144);    // local port
      user = u;
    }
    beat = rxd.read();
    inpkt = !beat.last;
    ap_uint<4> c = 0;                         // keep is contiguous from byte 0: 64-bit words carrying data
    for (int q = 0; q < 8; q++) {
#pragma HLS UNROLL
      if (beat.keep[8 * q]) c = q + 1;
    }
    n = c == 0 ? ap_uint<4>(1) : c;
    k = 0;
    have = true;
    nbeat++;
  }
  // emit one word per call, in the same call the beat was taken, and only when the pipeline input has room
  if (have && !toPipe.full()) {
    extendedAxiWord w;
    w.data = 0; w.keep = 0;
    for (int q = 0; q < 8; q++) {             // per-lane mux, not a 512-bit shifter
#pragma HLS UNROLL
      if (k == q) { w.data = beat.data.range(64 * q + 63, 64 * q); w.keep = beat.keep.range(8 * q + 7, 8 * q); }
    }
    w.user = user;
    const bool lastw = (k == n - 1);
    w.last = beat.last && lastw;
    toPipe.write(w);
    nword++;
    if (lastw) have = false; else k++;
  }
}

// TX: one 64-bit response word per call into a 512-bit beat; metadata at a response's first word. Blocking writes
// (no full() checks): hardware waits on the output, and a full() test against the next read made HLS miss II=1.
void tx_proc(hls::stream<extendedAxiWord> &fromPipe, hls::stream<meta_t> &txm, hls::stream<nax_t> &txd, hls::stream<dbg_t> &dbg)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static ap_uint<56> nw = 0, nr = 0;
  static dbg_tick_t tick = 0;
  if (++tick == 0) { dbg.write_nb(dbg_word(3, nw)); dbg.write_nb(dbg_word(4, nr)); }
  static ap_uint<64> lane[8];                 // 8 lane registers, each with its own enable (a 512-bit accumulator
  static ap_uint<8> lkeep[8];                 // with 8 conditional writes became a chain of 512-bit selects)
#pragma HLS ARRAY_PARTITION variable=lane complete
#pragma HLS ARRAY_PARTITION variable=lkeep complete
  static ap_uint<4> k = 0;
  static bool inpkt = false;
  extendedAxiWord w;
  if (!fromPipe.read_nb(w)) return;
  nw++;
  if (w.last) nr++;
  if (!inpkt) {
    meta_t m = 0;
    m.range(127, 96) = w.user.range(47, 16);   // client IPv4
    m.range(143, 128) = w.user.range(15, 0);   // client port
    m.range(159, 144) = w.user.range(63, 48);  // local port
    m.range(175, 160) = w.user.range(111, 96); // response length (bytes)
    txm.write(m);
    inpkt = true;
  }
  lane[k] = w.data;
  lkeep[k] = w.keep;
  if (w.last || k == 7) {
    nax_t o;
    for (int q = 0; q < 8; q++) {
#pragma HLS UNROLL
      const bool v = q <= k;                  // lanes above k belong to no word of this beat
      o.data.range(64 * q + 63, 64 * q) = v ? lane[q] : ap_uint<64>(0);
      o.keep.range(8 * q + 7, 8 * q) = v ? lkeep[q] : ap_uint<8>(0);
    }
    o.last = w.last;
    txd.write(o);
    k = 0;
    if (w.last) inpkt = false;
  } else k++;
}

// ---------------------------------------------------------------------------------------------------------------
// one memory command per call: a write (priority) or a read, as a burst over m_axi. A write command is held as pending
// until it is done; in C simulation the burst starts only once all its data words are queued (csim runs the dataflow
// processes one after another, so a blocking read of data not produced yet would deadlock the testbench -- hardware
// simply waits on the stream).
struct mem_state { memCtrlWord w; bool havew; ap_uint<56> nrc, nrw, nwc, nww; };   // + debug counters

void mem_svc(hls::stream<memCtrlWord> &rdCmd, hls::stream<ap_uint<512> > &rdData, hls::stream<memCtrlWord> &wrCmd,
             hls::stream<ap_uint<512> > &wrData, ap_uint<512> *mem, mem_state &st)
{
#pragma HLS INLINE off
  memCtrlWord c;
  if (!st.havew && wrCmd.read_nb(c)) { st.w = c; st.havew = true; }
  if (st.havew) {
    const unsigned n = st.w.count == 0 ? 1u : (unsigned)st.w.count;
#ifndef __SYNTHESIS__
    if (wrData.size() < n) return;
#endif
  wr:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
      mem[(unsigned)st.w.address + i] = wrData.read();
    }
    st.havew = false;
    st.nwc++; st.nww += n;
  } else if (rdCmd.read_nb(c)) {
    const unsigned n = c.count == 0 ? 1u : (unsigned)c.count;
  rd:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32
      rdData.write(mem[(unsigned)c.address + i]);
    }
    st.nrc++; st.nrw += n;
  }
}

// memCtrlWord {address 32, count 8} <-> a 64-bit stream word for the mcd -> mcd_mem kernel-to-kernel links
static inline ap_uint<64> pack_cmd(const memCtrlWord &c) { ap_uint<64> w = 0; w.range(31, 0) = c.address; w.range(39, 32) = c.count; return w; }
static inline memCtrlWord unpack_cmd(const ap_uint<64> &w) { memCtrlWord c; c.address = w.range(31, 0); c.count = w.range(39, 32); return c; }

template <int ID>
void cmd_out(hls::stream<memCtrlWord> &in, hls::stream<ap_uint<64> > &out, hls::stream<dbg_t> &dbg)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static ap_uint<56> n = 0;
  static dbg_tick_t tick = 0;
  if (++tick == 0) dbg.write_nb(dbg_word(ID, n));
  memCtrlWord c;
  if (!out.full() && in.read_nb(c)) { out.write(pack_cmd(c)); n++; }
}

// merges the per-process snapshot streams into the one debug output (non-blocking: never stalls the datapath)
void dbg_merge(hls::stream<dbg_t> &a, hls::stream<dbg_t> &b, hls::stream<dbg_t> &c, hls::stream<dbg_t> &d,
               hls::stream<dbg_t> &e, hls::stream<dbg_t> &f, hls::stream<dbg_t> &out)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static ap_uint<3> rr = 0;
  dbg_t w; bool got = false;
  switch (rr) {
    case 0: got = a.read_nb(w); break;
    case 1: got = b.read_nb(w); break;
    case 2: got = c.read_nb(w); break;
    case 3: got = d.read_nb(w); break;
    case 4: got = e.read_nb(w); break;
    default: got = f.read_nb(w); break;
  }
  rr = rr == 5 ? ap_uint<3>(0) : ap_uint<3>(rr + 1);
  if (got) out.write_nb(w);
}

// mcd_mem: one memory command per call per memory (csim calls mcd_mem_step; the kernel loops it forever)
struct mcd_mem_state { mem_state ht, vs; ap_uint<56> loops; dbg_tick_t tick; };
void mcd_mem_step(hls::stream<ap_uint<64> > &htRdCmd, hls::stream<ap_uint<512> > &htRdData, hls::stream<ap_uint<64> > &htWrCmd,
                  hls::stream<ap_uint<512> > &htWrData, hls::stream<ap_uint<64> > &vsRdCmd, hls::stream<ap_uint<512> > &vsRdData,
                  hls::stream<ap_uint<64> > &vsWrCmd, hls::stream<ap_uint<512> > &vsWrData, ap_uint<512> *ht, ap_uint<512> *vs,
                  hls::stream<dbg_t> &dbg, mcd_mem_state &st)
{
#pragma HLS INLINE
  // unpack into local command streams, then the shared one-command server
  static hls::stream<memCtrlWord> hr("hr"), hw("hw"), vr("vr"), vw("vw");
#pragma HLS STREAM variable=hr depth=4
#pragma HLS STREAM variable=hw depth=4
#pragma HLS STREAM variable=vr depth=4
#pragma HLS STREAM variable=vw depth=4
  ap_uint<64> w;
  if (!hr.full() && htRdCmd.read_nb(w)) hr.write(unpack_cmd(w));
  if (!hw.full() && htWrCmd.read_nb(w)) hw.write(unpack_cmd(w));
  if (!vr.full() && vsRdCmd.read_nb(w)) vr.write(unpack_cmd(w));
  if (!vw.full() && vsWrCmd.read_nb(w)) vw.write(unpack_cmd(w));
  mem_svc(hr, htRdData, hw, htWrData, ht, st.ht);
  mem_svc(vr, vsRdData, vw, vsWrData, vs, st.vs);
  st.loops++;
  if (++st.tick == 0) {
    dbg.write_nb(dbg_word(16, st.ht.nrc)); dbg.write_nb(dbg_word(17, st.ht.nrw)); dbg.write_nb(dbg_word(18, st.ht.nwc)); dbg.write_nb(dbg_word(19, st.ht.nww));
    dbg.write_nb(dbg_word(20, st.vs.nrc)); dbg.write_nb(dbg_word(21, st.vs.nrw)); dbg.write_nb(dbg_word(22, st.vs.nwc)); dbg.write_nb(dbg_word(23, st.vs.nww));
    dbg.write_nb(dbg_word(24, st.loops));
  }
}

// One free-running server per memory (2026-10-15). They used to share one sequential loop (mcd_mem_step), so a hash-
// table read blocked on a full htRdData stream also stopped the value-store server: with ~128 requests in flight the
// value store stalled, the FIFOs between hash table and value store filled, the hash table stopped draining htRdData,
// and the pipeline deadlocked (no reply until the xclbin was reloaded). Now each server blocks only on its own streams.
// (C simulation still uses mcd_mem_step via mcd_mem_tb_step.)
static void mem_server(hls::stream<ap_uint<64> > &rdCmd, hls::stream<ap_uint<512> > &rdData, hls::stream<ap_uint<64> > &wrCmd,
                       hls::stream<ap_uint<512> > &wrData, ap_uint<512> *mem, hls::stream<dbg_t> &dbg, const unsigned id0)
{
#pragma HLS INLINE off
  hls::stream<memCtrlWord> r("r"), w("w");
#pragma HLS STREAM variable=r depth=4
#pragma HLS STREAM variable=w depth=4
  mem_state st = {};
  ap_uint<56> loops = 0; dbg_tick_t tick = 0;
srv:
  while (1) {
    ap_uint<64> c;
    if (!r.full() && rdCmd.read_nb(c)) r.write(unpack_cmd(c));
    if (!w.full() && wrCmd.read_nb(c)) w.write(unpack_cmd(c));
    mem_svc(r, rdData, w, wrData, mem, st);
    loops++;
    if (++tick == 0) {
      dbg.write_nb(dbg_word(id0, st.nrc)); dbg.write_nb(dbg_word(id0 + 1, st.nrw));
      dbg.write_nb(dbg_word(id0 + 2, st.nwc)); dbg.write_nb(dbg_word(id0 + 3, st.nww));
      if (id0 == 16) dbg.write_nb(dbg_word(24, loops));
    }
  }
}

static void dbg_merge2(hls::stream<dbg_t> &a, hls::stream<dbg_t> &b, hls::stream<dbg_t> &out)
{
#pragma HLS INLINE off
mrg:
  while (1) {
#pragma HLS PIPELINE II=1
    dbg_t v;
    if (a.read_nb(v)) out.write_nb(v);
    else if (b.read_nb(v)) out.write_nb(v);
  }
}

extern "C" void mcd_mem(hls::stream<ap_uint<64> > &htRdCmd, hls::stream<ap_uint<512> > &htRdData, hls::stream<ap_uint<64> > &htWrCmd,
                        hls::stream<ap_uint<512> > &htWrData, hls::stream<ap_uint<64> > &vsRdCmd, hls::stream<ap_uint<512> > &vsRdData,
                        hls::stream<ap_uint<64> > &vsWrCmd, hls::stream<ap_uint<512> > &vsWrData, ap_uint<512> *ht, ap_uint<512> *vs,
                        hls::stream<dbg_t> &dbg)
{
#pragma HLS INTERFACE axis port=dbg
#pragma HLS INTERFACE axis port=htRdCmd
#pragma HLS INTERFACE axis port=htRdData
#pragma HLS INTERFACE axis port=htWrCmd
#pragma HLS INTERFACE axis port=htWrData
#pragma HLS INTERFACE axis port=vsRdCmd
#pragma HLS INTERFACE axis port=vsRdData
#pragma HLS INTERFACE axis port=vsWrCmd
#pragma HLS INTERFACE axis port=vsWrData
#pragma HLS INTERFACE m_axi port=ht offset=slave bundle=gmem0 depth=1024 max_read_burst_length=32 max_write_burst_length=32 num_read_outstanding=4 num_write_outstanding=4
#pragma HLS INTERFACE m_axi port=vs offset=slave bundle=gmem1 depth=1048576 max_read_burst_length=32 max_write_burst_length=32 num_read_outstanding=4 num_write_outstanding=4
#pragma HLS INTERFACE s_axilite port=ht
#pragma HLS INTERFACE s_axilite port=vs
#pragma HLS INTERFACE s_axilite port=return
  // started once by the host (after it has the buffers) and never returns; a new xclbin load resets it
#pragma HLS DATAFLOW
  static hls::stream<dbg_t> dh("dh"), dv("dv");
#pragma HLS STREAM variable=dh depth=8
#pragma HLS STREAM variable=dv depth=8
  mem_server(htRdCmd, htRdData, htWrCmd, htWrData, ht, dh, 16);
  mem_server(vsRdCmd, vsRdData, vsWrCmd, vsWrData, vs, dv, 20);
  dbg_merge2(dh, dv, dbg);
}

// mcd_dbg: host-started, never returns; stores every debug snapshot word at out[id] (ids < 64)
extern "C" void mcd_dbg(hls::stream<dbg_t> &a, hls::stream<dbg_t> &b, ap_uint<64> *out)
{
#pragma HLS INTERFACE axis port=a
#pragma HLS INTERFACE axis port=b
#pragma HLS INTERFACE m_axi port=out offset=slave bundle=gmem0 depth=64 max_write_burst_length=2 num_write_outstanding=4
#pragma HLS INTERFACE s_axilite port=out
#pragma HLS INTERFACE s_axilite port=return
store:
  while (1) {
    dbg_t w;
    if (a.read_nb(w)) out[(unsigned)w.range(61, 56)] = w;
    if (b.read_nb(w)) out[(unsigned)w.range(61, 56)] = w;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// value-store slot allocator: returned slots -> recycled (one process), recycled first else fresh -> the pipeline
// (another); a returned address of all-ones never occurs (slots are < VS_SLOTS * SLOT_LINES) -- forwarding it to the
// flash free-address stream only gives that stream the producer dataflow requires; no flash address is handed out
void alloc_ret_proc(hls::stream<ap_uint<32> > &ret, hls::stream<ap_uint<32> > &recycled, hls::stream<ap_uint<32> > &flashAssign)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  ap_uint<32> a;
  if (recycled.full() || flashAssign.full() || !ret.read_nb(a)) return;
  if (a == 0xFFFFFFFF) flashAssign.write(a); else recycled.write(a);
}

void alloc_assign_proc(hls::stream<ap_uint<32> > &recycled, hls::stream<ap_uint<32> > &assign)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static ap_uint<16> fresh = 0;
  ap_uint<32> a;
  if (assign.full()) return;                  // nothing consumes free slots before a SET: never block on it
  if (recycled.read_nb(a)) assign.write(a);
  else if (fresh < VS_SLOTS) { assign.write(ap_uint<32>(fresh) * SLOT_LINES); fresh++; }
}

void flash_stub_step(hls::stream<flashMemCtrlWord> &rdCmd, hls::stream<ap_uint<64> > &rdData,
                     hls::stream<flashMemCtrlWord> &wrCmd, hls::stream<ap_uint<64> > &wrData, ap_uint<13> &rdLeft)
{
#pragma HLS INLINE
  flashMemCtrlWord c;
  ap_uint<64> d;
  wrData.read_nb(d);
  wrCmd.read_nb(c);
  if (rdLeft != 0) { if (!rdData.full()) { rdData.write(0); rdLeft--; } }
  else if (rdCmd.read_nb(c)) rdLeft = c.count;
}

// the small free-running glue processes, one call = one cycle, each its own dataflow process (pipelined II=1)
void flash_proc(hls::stream<flashMemCtrlWord> &fRdCmd, hls::stream<ap_uint<64> > &fRdData,
                hls::stream<flashMemCtrlWord> &fWrCmd, hls::stream<ap_uint<64> > &fWrData)
{
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1 style=flp
  static ap_uint<13> fleft = 0;
  flash_stub_step(fRdCmd, fRdData, fWrCmd, fWrData, fleft);
}

// ---------------------------------------------------------------------------------------------------------------
extern "C" void mcd(hls::stream<meta_t> &rxm, hls::stream<nax_t> &rxd, hls::stream<meta_t> &txm,
                    hls::stream<nax_t> &txd,
                    hls::stream<ap_uint<64> > &htRdCmdO, hls::stream<ap_uint<512> > &htRdData, hls::stream<ap_uint<64> > &htWrCmdO,
                    hls::stream<ap_uint<512> > &htWrData, hls::stream<ap_uint<64> > &vsRdCmdO, hls::stream<ap_uint<512> > &vsRdData,
                    hls::stream<ap_uint<64> > &vsWrCmdO, hls::stream<ap_uint<512> > &vsWrData, hls::stream<dbg_t> &dbgO)
{
#pragma HLS INTERFACE axis port=dbgO
#pragma HLS INTERFACE axis port=rxm
#pragma HLS INTERFACE axis port=rxd
#pragma HLS INTERFACE axis port=txm
#pragma HLS INTERFACE axis port=txd
#pragma HLS INTERFACE axis port=htRdCmdO
#pragma HLS INTERFACE axis port=htRdData
#pragma HLS INTERFACE axis port=htWrCmdO
#pragma HLS INTERFACE axis port=htWrData
#pragma HLS INTERFACE axis port=vsRdCmdO
#pragma HLS INTERFACE axis port=vsRdData
#pragma HLS INTERFACE axis port=vsWrCmdO
#pragma HLS INTERFACE axis port=vsWrData
#pragma HLS INTERFACE ap_ctrl_none port=return
// v4 (2026-10-17): no start propagation -- every process free-running (ap_start = 1), as the 2015 design assumes. With
// start FIFOs, a process may only iterate when its stream producer does: cmd_out<5> ran once per memRead iteration, so
// when memRead blocked on a full htRdCmd, nothing drained htRdCmd -- the core deadlocked under load (hpy440 3 x 21 and
// 4 x 16 GETs in flight) and the start FIFOs then froze every process, rx_proc included.
#pragma HLS DATAFLOW disable_start_propagation

  static hls::stream<extendedAxiWord> toPipe("toPipe"), fromPipe("fromPipe");
  static hls::stream<pipelineWord> rp2ht("rp2ht"), ht2sp("ht2sp"), mg2rf("mg2rf"), sp2fl("sp2fl"), sp2dr("sp2dr"),
                                   fl2mg("fl2mg"), dr2mg("dr2mg");
  static hls::stream<memCtrlWord> htRdCmd("htRdCmd"), htWrCmd("htWrCmd"), vsRdCmd("vsRdCmd"), vsWrCmd("vsWrCmd");
  static hls::stream<flashMemCtrlWord> fRdCmd("fRdCmd"), fWrCmd("fWrCmd");
  static hls::stream<ap_uint<64> > fRdData("fRdData"), fWrData("fWrData");
  static hls::stream<ap_uint<32> > addrRet("addrRet"), addrDram("addrDram"), addrFlash("addrFlash"), recycled("recycled");
#pragma HLS STREAM variable=recycled depth=RET_DEPTH
#pragma HLS STREAM variable=toPipe depth=16
#pragma HLS STREAM variable=fromPipe depth=16
#pragma HLS STREAM variable=rp2ht depth=16
#pragma HLS STREAM variable=ht2sp depth=16
#pragma HLS STREAM variable=mg2rf depth=16
#pragma HLS STREAM variable=sp2fl depth=16
#pragma HLS STREAM variable=sp2dr depth=16
#pragma HLS STREAM variable=fl2mg depth=16
#pragma HLS STREAM variable=dr2mg depth=16
#pragma HLS STREAM variable=htRdCmd depth=16
#pragma HLS STREAM variable=htWrCmd depth=16
#pragma HLS STREAM variable=vsRdCmd depth=16
#pragma HLS STREAM variable=vsWrCmd depth=16
#pragma HLS STREAM variable=addrDram depth=16
  static ap_uint<1> flushReq = 0, flushDone = 0;
  const ap_uint<1> flushAck = 1;

  static hls::stream<dbg_t> d_rx("d_rx"), d_tx("d_tx"), d_hr("d_hr"), d_hw("d_hw"), d_vr("d_vr"), d_vw("d_vw");
#pragma HLS STREAM variable=d_rx depth=4
#pragma HLS STREAM variable=d_tx depth=4
#pragma HLS STREAM variable=d_hr depth=4
#pragma HLS STREAM variable=d_hw depth=4
#pragma HLS STREAM variable=d_vr depth=4
#pragma HLS STREAM variable=d_vw depth=4
  rx_proc(rxm, rxd, toPipe, d_rx);
  tx_proc(fromPipe, txm, txd, d_tx);
  alloc_ret_proc(addrRet, recycled, addrFlash);
  alloc_assign_proc(recycled, addrDram);
  flash_proc(fRdCmd, fRdData, fWrCmd, fWrData);
  binaryParser(toPipe, rp2ht);
  hashTable(rp2ht, ht2sp, htRdData, htRdCmd, htWrData, htWrCmd, addrRet, addrDram, addrFlash, flushReq, flushAck, flushDone);
  splitter(ht2sp, sp2fl, sp2dr);
  valueStoreDram(sp2dr, vsRdCmd, vsRdData, vsWrCmd, vsWrData, dr2mg);
  valueStoreFlash(sp2fl, fRdCmd, fRdData, fWrCmd, fWrData, fl2mg);
  merger(fl2mg, dr2mg, mg2rf);
  binaryResponse(mg2rf, fromPipe);
  cmd_out<5>(htRdCmd, htRdCmdO, d_hr);
  cmd_out<6>(htWrCmd, htWrCmdO, d_hw);
  cmd_out<7>(vsRdCmd, vsRdCmdO, d_vr);
  cmd_out<8>(vsWrCmd, vsWrCmdO, d_vw);
  dbg_merge(d_rx, d_tx, d_hr, d_hw, d_vr, d_vw, dbgO);
}

#ifndef __SYNTHESIS__
// testbench hook: one mcd_mem step with its own state (the kernel itself loops forever)
void mcd_mem_tb_step(hls::stream<ap_uint<64> > &htRdCmd, hls::stream<ap_uint<512> > &htRdData, hls::stream<ap_uint<64> > &htWrCmd,
                     hls::stream<ap_uint<512> > &htWrData, hls::stream<ap_uint<64> > &vsRdCmd, hls::stream<ap_uint<512> > &vsRdData,
                     hls::stream<ap_uint<64> > &vsWrCmd, hls::stream<ap_uint<512> > &vsWrData, ap_uint<512> *ht, ap_uint<512> *vs,
                     hls::stream<dbg_t> &dbg)
{
  static mcd_mem_state st = {};
  mcd_mem_step(htRdCmd, htRdData, htWrCmd, htWrData, vsRdCmd, vsRdData, vsWrCmd, vsWrData, ht, vs, dbg, st);
}
#endif
