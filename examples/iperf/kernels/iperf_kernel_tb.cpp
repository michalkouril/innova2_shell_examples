// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// csim for iperf_srv: replays iperf2 UDP datagrams (iperf 2.2.1 framing: UDP_datagram id / tv_sec / tv_usec / id2,
// big-endian, 64-bit sequence id2:id, negative on the final datagram) as FLD-lite frames, then checks every report
// frame byte for byte against a reference computed here from the datagrams sent (iperf2's server accounting, written
// out independently): headers swapped, IPv4 / UDP lengths, the zeroed UDP_datagram and the 112-byte server_hdr.
//   test 1: 100 datagrams of 1470 B with 4 lost, 2 swapped pairs (out of order), 1 duplicate, varying spacing (jitter);
//           its final datagram twice (the client's retry) -> the same report twice
//   test 2: 40 datagrams of 64 B, no loss, without any reset in between -> its own report
//   also:   a UDP frame to another port and a non-IPv4 frame -> dropped and counted
//   build: g++ -O1 -std=c++14 -I<Vitis_HLS>/include -o iperf_tb iperf_kernel_tb.cpp iperf_kernel.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>

typedef ap_axiu<512, 32, 0, 0> pkt_t;
typedef ap_axiu<16, 0, 0, 0> req_t;
typedef ap_axiu<64, 0, 0, 0> tick_t;
typedef ap_axiu<64, 0, 0, 0> scmd_t;
typedef ap_axiu<32, 0, 0, 0> cnt_t;
extern uint64_t iperf_tb_now;
extern "C" void iperf_sctl(unsigned op, unsigned a, unsigned b, uint32_t *out, hls::stream<scmd_t> &scfg,
                           hls::stream<req_t> &srq, hls::stream<cnt_t> &scnt);
struct srv_state;
// the testbench drives iperf_srv's per-turn function through this hook (the kernel itself loops forever)
void iperf_srv_tb_turn(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, hls::stream<scmd_t> &scfg,
                       hls::stream<req_t> &srq, hls::stream<cnt_t> &scnt);

static const uint32_t CPU = 300;   // cycles per microsecond (the default)
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void be32(std::vector<uint8_t> &f, int o, uint32_t v) { f[o] = v >> 24; f[o + 1] = v >> 16; f[o + 2] = v >> 8; f[o + 3] = v; }
static uint32_t rbe32(const std::vector<uint8_t> &f, int o) { return (f[o] << 24) | (f[o + 1] << 16) | (f[o + 2] << 8) | f[o + 3]; }

// an Ethernet / IPv4 / UDP frame carrying an iperf2 datagram of payload length pl
static std::vector<uint8_t> frame(int64_t seq, uint32_t sec, uint32_t usec, int pl, unsigned dport = 5001, bool ipv4 = true)
{
  std::vector<uint8_t> f(42 + pl, 0);
  const uint8_t dm[6] = {0x02, 0, 0, 0, 0, 2}, sm[6] = {0x02, 0, 0, 0, 0, 1};
  memcpy(&f[0], dm, 6); memcpy(&f[6], sm, 6);
  f[12] = ipv4 ? 0x08 : 0x86; f[13] = ipv4 ? 0x00 : 0xdd;
  f[14] = 0x45; f[16] = (20 + 8 + pl) >> 8; f[17] = (20 + 8 + pl) & 0xff; f[22] = 64; f[23] = 17;
  f[26] = 192; f[27] = 168; f[28] = 77; f[29] = 1; f[30] = 192; f[31] = 168; f[32] = 77; f[33] = 2;
  f[34] = 40000 >> 8; f[35] = 40000 & 0xff; f[36] = dport >> 8; f[37] = dport & 0xff; f[38] = (8 + pl) >> 8; f[39] = (8 + pl) & 0xff;
  be32(f, 42, (uint32_t)seq); be32(f, 46, sec); be32(f, 50, usec); be32(f, 54, (uint32_t)((uint64_t)seq >> 32));
  for (int i = 16; i < pl; i++) f[42 + i] = (uint8_t)(i * 7);
  return f;
}

static void push(hls::stream<pkt_t> &in, const std::vector<uint8_t> &f)
{
  for (size_t o = 0; o < f.size(); o += 64) {
    pkt_t p; p.data = 0; p.keep = 0; p.strb = -1; p.user = 0; p.user.range(15, 0) = f.size();
    for (size_t j = 0; j < 64 && o + j < f.size(); j++) { p.data.range(8 * j + 7, 8 * j) = f[o + j]; p.keep[j] = 1; }
    p.last = o + 64 >= f.size();
    in.write(p);
  }
}

static std::vector<uint8_t> pull(hls::stream<pkt_t> &out, unsigned &len)
{
  std::vector<uint8_t> f; bool last = false; len = 0;
  while (!last && !out.empty()) {
    pkt_t p = out.read(); len = p.user.range(15, 0);
    for (int j = 0; j < 64; j++) if (p.keep[j]) f.push_back((uint8_t)p.data.range(8 * j + 7, 8 * j));
    last = p.last;
  }
  return f;
}

// the reference: iperf2's server accounting over the datagrams as they arrive (seq, arrival cycles, sent us, payload)
struct ref_t { int64_t maxid = 0; uint64_t bytes = 0, first = 0, last = 0, jq4 = 0; uint32_t lost = 0, ooo = 0, rx = 0; int64_t pt = 0; bool havet = false, start = false; };
static void ref_add(ref_t &r, int64_t seq, uint64_t now, uint64_t sent_us, int pl)
{
  if (!r.start) { r.start = true; r.first = now; }
  r.last = now; r.bytes += pl; r.rx++;
  if (seq != r.maxid + 1) { if (seq < r.maxid + 1) r.ooo++; else r.lost += (uint32_t)(seq - r.maxid - 1); }
  if (seq > r.maxid) r.maxid = seq;
  const int64_t transit = (int64_t)now - (int64_t)(sent_us * CPU);
  if (r.havet) { int64_t d = transit - r.pt; if (d < 0) d = -d; r.jq4 = r.jq4 + (uint64_t)d - (r.jq4 >> 4); }
  r.pt = transit; r.havet = true;
}

static void check_report(const std::vector<uint8_t> &f, unsigned len, const ref_t &r, const std::vector<uint8_t> &req, const char *tag)
{
  printf("%s: report frame %zu B (user length %u)\n", tag, f.size(), len);
  CHECK(f.size() == 170 && len == 170, "%s: report length %zu / %u, want 170", tag, f.size(), len);
  if (f.size() != 170) return;
  for (int i = 0; i < 6; i++) CHECK(f[i] == req[6 + i] && f[6 + i] == req[i], "%s: MAC byte %d", tag, i);
  for (int i = 0; i < 4; i++) CHECK(f[26 + i] == req[30 + i] && f[30 + i] == req[26 + i], "%s: IP byte %d", tag, i);
  CHECK(f[34] == req[36] && f[35] == req[37] && f[36] == req[34] && f[37] == req[35], "%s: ports", tag);
  CHECK(((f[16] << 8) | f[17]) == 156 && ((f[38] << 8) | f[39]) == 136, "%s: IPv4 / UDP length", tag);
  CHECK(f[24] == 0 && f[25] == 0 && f[40] == 0 && f[41] == 0, "%s: checksums left to the NIC", tag);
  for (int i = 42; i < 58; i++) CHECK(f[i] == 0, "%s: UDP_datagram byte %d not zero", tag, i - 42);
  const int h = 58;                     // server_hdr
  const uint64_t dur = r.last - r.first, fq = (uint64_t)CPU * 1000000;
  const uint64_t err = r.lost > r.ooo ? r.lost - r.ooo : 0, jus = (r.jq4 >> 4) / CPU;
  const uint32_t want[28] = {0xC8000000u, (uint32_t)(r.bytes >> 32), (uint32_t)r.bytes, (uint32_t)(dur / fq),
                             (uint32_t)((dur % fq) / CPU), (uint32_t)err, r.ooo, (uint32_t)r.maxid,
                             (uint32_t)(jus / 1000000), (uint32_t)(jus % 1000000)};
  uint32_t w[28];
  for (int i = 0; i < 28; i++) w[i] = rbe32(f, h + 4 * i);
  uint32_t full[28] = {0};
  memcpy(full, want, sizeof(want));
  full[10] = 0x7fffffff; full[23] = dur ? (uint32_t)((uint64_t)r.rx * fq / dur) : 0; full[24] = 1;
  full[25] = (uint32_t)(err >> 32); full[27] = (uint32_t)((uint64_t)r.maxid >> 32);
  for (int i = 0; i < 28; i++) CHECK(w[i] == full[i], "%s: server_hdr word %d = 0x%08x, want 0x%08x", tag, i, w[i], full[i]);
  printf("  flags 0x%08x bytes %llu duration %u.%06u s lost %u/%u out-of-order %u jitter %u.%06u s pps %u\n", w[0],
         (unsigned long long)(((uint64_t)w[1] << 32) | w[2]), w[3], w[4], w[5], w[7], w[6], w[8], w[9], w[23]);
}

int main()
{
  hls::stream<pkt_t> in("in"), out("out");
  hls::stream<scmd_t> scfg("scfg"); hls::stream<req_t> srq("srq"); hls::stream<cnt_t> scnt("scnt");
  auto run = [&]() { while (!in.empty()) iperf_srv_tb_turn(in, out, scfg, srq, scnt); };
  iperf_tb_now = 1000;

  // test 1: 100 datagrams, 1470 B; lost 17 23 61 88; arrival order swaps (40,41) and (70,71); 55 sent twice
  ref_t r1; std::vector<int64_t> order;
  for (int64_t s = 1; s <= 100; s++) {
    if (s == 17 || s == 23 || s == 61 || s == 88) continue;
    order.push_back(s);
    if (s == 55) order.push_back(55);
  }
  for (size_t i = 0; i + 1 < order.size(); i++)
    if (order[i] == 40 || order[i] == 70) { std::swap(order[i], order[i + 1]); i++; }
  uint64_t sent_us = 1700000000ull * 1000000;   // the client's clock, in microseconds
  for (size_t i = 0; i < order.size(); i++) {
    const int64_t s = order[i];
    const uint64_t su = sent_us + (uint64_t)s * 10;
    iperf_tb_now += 3000 + (i % 5) * 37;       // arrival spacing varies: jitter
    push(in, frame(s, su / 1000000, su % 1000000, 1470));
    run();
    ref_add(r1, s, iperf_tb_now, su, 1470);
  }
  CHECK(out.empty(), "data datagrams must not be echoed");
  const std::vector<uint8_t> fin1 = frame(-101, 0, 0, 1470);
  unsigned l1, l2;
  iperf_tb_now += 5000; push(in, fin1); run(); std::vector<uint8_t> rep1 = pull(out, l1);
  iperf_tb_now += 5000; push(in, fin1); run(); std::vector<uint8_t> rep1b = pull(out, l2);
  check_report(rep1, l1, r1, fin1, "test 1");
  CHECK(rep1 == rep1b, "test 1: the retried final datagram must get the same report");

  // drops
  push(in, frame(5, 0, 0, 100, 123)); push(in, frame(5, 0, 0, 100, 5001, false)); run();
  CHECK(out.empty(), "dropped frames must not be answered");

  // test 2: 40 datagrams of 64 B, no loss, no reset in between
  ref_t r2;
  for (int64_t s = 1; s <= 40; s++) {
    const uint64_t su = sent_us + 5000000 + (uint64_t)s * 2;
    iperf_tb_now += 600;
    push(in, frame(s, su / 1000000, su % 1000000, 64)); run();
    ref_add(r2, s, iperf_tb_now, su, 64);
  }
  const std::vector<uint8_t> fin2 = frame(-41, 0, 0, 64);
  iperf_tb_now += 600; push(in, fin2); run(); std::vector<uint8_t> rep2 = pull(out, l1);
  check_report(rep2, l1, r2, fin2, "test 2");

  // the host's counters through iperf_sctl
  uint32_t c[16] = {0};
  hls::stream<cnt_t> dummy;
  // iperf_sctl's request is answered by a turn of iperf_srv: queue the request, turn, then read (C simulation runs
  // one process at a time)
  { req_t q; q.data = 1; q.keep = -1; q.strb = -1; q.last = 1; srq.write(q); }
  iperf_srv_tb_turn(in, out, scfg, srq, scnt);
  for (int i = 0; i < 16; i++) c[i] = scnt.read().data;
  printf("counters: frames %u data %u fin %u reports %u drop %u tests %u | last test rx %u bytes %u maxid %u lost %u ooo %u dup %u\n",
         c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[9], c[10], c[11], c[12]);
  CHECK(c[0] == order.size() + 2 + 2 + 40 + 1, "frames %u", c[0]);
  CHECK(c[1] == order.size() + 40 && c[2] == 3 && c[3] == 3 && c[4] == 2 && c[5] == 2, "cumulative counters");
  CHECK(c[6] == 40 && c[7] == 40 * 64 && c[9] == 40 && c[10] == 0 && c[11] == 0, "test 2 counters");
  // a SET through the real iperf_sctl (no reply needed)
  uint32_t o[16];
  iperf_sctl(1, 0, 6001, o, scfg, srq, scnt);
  iperf_srv_tb_turn(in, out, scfg, srq, scnt);
  push(in, frame(1, 0, 0, 100, 5001)); run();
  { req_t q; q.data = 1; q.keep = -1; q.strb = -1; q.last = 1; srq.write(q); }
  iperf_srv_tb_turn(in, out, scfg, srq, scnt);
  for (int i = 0; i < 16; i++) c[i] = scnt.read().data;
  CHECK(c[4] == 3, "after SET port 6001 a datagram to 5001 is dropped (drop %u)", c[4]);
  printf("iperf_srv csim: %s (%d problems)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
