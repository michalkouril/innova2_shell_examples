// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ntp_kernel_tb -- C testbench for ntp_kernel.cpp (HLS C-simulation semantics, plain g++):
//   g++ -O1 -I<Vitis_HLS>/include -o ntp_tb ntp_kernel_tb.cpp && ./ntp_tb       -> "NTP-TB PASS"
// ntp_resp: valid v4 and v3 requests are answered byte-exactly as a reference reply built here (fields, origin, T2 / T3
// with the calibration constants); short, long, non-123, non-IPv4, IP-options, mode 1 and version 2 / 5 frames are
// dropped and counted in the right counter. ntp_tod: rate, step and snapshot, against a 96-bit model.
#include "ntp_kernel.cpp"
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> request(int vn, int mode, uint64_t xmt, int len = 90)
{
  std::vector<uint8_t> f(len, 0);
  const uint8_t dmac[6] = {0x02, 0, 0, 0, 0, 0xaa}, smac[6] = {0x02, 0, 0, 0, 0, 0xbb};
  memcpy(&f[0], dmac, 6); memcpy(&f[6], smac, 6); f[12] = 0x08; f[13] = 0x00;
  f[14] = 0x45; f[16] = 0; f[17] = 76; f[22] = 64; f[23] = 17; f[24] = 0x12; f[25] = 0x34;
  const uint8_t sip[4] = {192, 168, 77, 1}, dip[4] = {192, 168, 77, 2};
  memcpy(&f[26], sip, 4); memcpy(&f[30], dip, 4);
  f[34] = 0xc3; f[35] = 0x50; f[36] = 0; f[37] = 123; f[38] = 0; f[39] = 56; f[40] = 0xab; f[41] = 0xcd;
  if (len >= 90) {
    f[42] = (0 << 6) | (vn << 3) | mode; f[43] = 0; f[44] = 6; f[45] = 0xec;
    for (int i = 0; i < 8; i++) f[42 + 40 + i] = (uint8_t)(xmt >> (56 - 8 * i));
  }
  return f;
}

static void send(hls::stream<pkt_t> &in, const std::vector<uint8_t> &f)
{
  const int n = (int)f.size(), nb = (n + 63) / 64;
  for (int b = 0; b < nb; b++) {
    pkt_t p; p.data = 0; p.keep = 0; p.strb = 0;
    for (int i = 0; i < 64 && b * 64 + i < n; i++) { p.data.range(8 * i + 7, 8 * i) = f[b * 64 + i]; p.keep[i] = 1; p.strb[i] = 1; }
    p.user = n; p.last = b == nb - 1;
    in.write(p);
  }
}

static std::vector<uint8_t> recv(hls::stream<pkt_t> &out)
{
  std::vector<uint8_t> f;
  for (;;) {
    pkt_t p = out.read();
    const int n = p.user.range(15, 0);
    for (int i = 0; i < 64 && (int)f.size() < n; i++) f.push_back(p.data.range(8 * i + 7, 8 * i));
    if (p.last) break;
  }
  return f;
}

static void put64(std::vector<uint8_t> &f, int o, uint64_t v) { for (int i = 0; i < 8; i++) f[o + i] = (uint8_t)(v >> (56 - 8 * i)); }
static void put32(std::vector<uint8_t> &f, int o, uint32_t v) { for (int i = 0; i < 4; i++) f[o + i] = (uint8_t)(v >> (24 - 8 * i)); }

int main()
{
  hls::stream<pkt_t> in("in"), out("out");
  hls::stream<req_t> trq("trq"), rrq("rrq");
  hls::stream<tick_t> ttk("ttk");
  hls::stream<rcmd_t> rcfg("rcfg");
  hls::stream<cnt_t> rcnt("rcnt");
  resp_state s;
  resp_init(s);
  auto set = [&](unsigned k, uint32_t v) { rcmd_t c; c.data = 0; c.data.range(7, 0) = k; c.data.range(63, 32) = v; rcfg.write(c);
                                           resp_step(in, out, trq, ttk, rcfg, rrq, rcnt, s); };
  set(RF_LEAP, 0); set(RF_STRATUM, 2); set(RF_PREC, (uint32_t)-28); set(RF_RDELAY, 0x00000123); set(RF_RDISP, 0x00000456);
  set(RF_REFID, 0x4c4f434c); set(RF_REF_LO, 0x89abcdef); set(RF_REF_HI, 0xe9000001); set(RF_CAL_RX, 100); set(RF_CAL_TX, 200);

  // two valid requests (v4, v3)
  const uint64_t T2 = 0xe9000002000010ffull, T3 = 0xe900000200002000ull;
  for (int vn = 4; vn >= 3; vn--) {
    const uint64_t xmt = 0x1122334455667788ull + vn;
    auto q = request(vn, 3, xmt);
    send(in, q);
    tick_t t; t.data = T2; ttk.write(t); t.data = T3; ttk.write(t);
    resp_step(in, out, trq, ttk, rcfg, rrq, rcnt, s);
    CHECK(trq.size() == 2, "v%d: %d timestamp requests", vn, (int)trq.size());
    while (!trq.empty()) trq.read();
    CHECK(out.size() == 2, "v%d: reply of %d beats", vn, (int)out.size());
    auto r = recv(out);
    auto e = q;                                     // the reference reply
    for (int i = 0; i < 6; i++) { e[i] = q[6 + i]; e[6 + i] = q[i]; }
    for (int i = 0; i < 4; i++) { e[26 + i] = q[30 + i]; e[30 + i] = q[26 + i]; }
    for (int i = 0; i < 2; i++) { e[34 + i] = q[36 + i]; e[36 + i] = q[34 + i]; }
    e[24] = e[25] = e[40] = e[41] = 0;
    e[42] = (0 << 6) | (vn << 3) | 4; e[43] = 2; e[44] = 6; e[45] = 0xe4;
    put32(e, 46, 0x123); put32(e, 50, 0x456); put32(e, 54, 0x4c4f434c); put64(e, 58, 0xe900000189abcdefull);
    put64(e, 66, xmt); put64(e, 74, T2 - 100); put64(e, 82, T3 + 200);
    CHECK(r == e, "v%d: reply differs", vn);
    if (r != e) for (size_t i = 0; i < r.size() && i < e.size(); i++) if (r[i] != e[i]) printf("  byte %zu: %02x want %02x\n", i, r[i], e[i]);
  }

  // drops: each frame asks for T2 only, emits nothing
  struct { std::vector<uint8_t> f; int ctr; const char *what; } bad[] = {
    {request(4, 3, 1, 60), C_DROP_LEN, "60 B frame"},
    {request(4, 3, 1, 140), C_DROP_LEN, "140 B frame (extension field)"},
    {request(4, 1, 1), C_DROP_MODE, "mode 1"},
    {request(2, 3, 1), C_DROP_MODE, "version 2"},
    {request(5, 3, 1), C_DROP_MODE, "version 5"},
    {request(4, 4, 1), C_DROP_MODE, "mode 4 (a reply)"},
  };
  auto port = request(4, 3, 1); port[37] = 124;
  auto ipo = request(4, 3, 1); ipo[14] = 0x46;
  auto arp = request(4, 3, 1); arp[12] = 0x08; arp[13] = 0x06;
  auto tcp = request(4, 3, 1); tcp[23] = 6;
  std::vector<std::pair<std::vector<uint8_t>, int>> all;
  for (auto &b : bad) all.push_back({b.f, b.ctr});
  for (auto *f : {&port, &ipo, &arp, &tcp}) all.push_back({*f, C_DROP_HDR});
  uint32_t want[NCNT] = {0};
  want[C_FRAMES] = 2; want[C_REPLIES] = 2;
  for (auto &a : all) {
    send(in, a.first);
    tick_t t; t.data = 7; ttk.write(t);
    resp_step(in, out, trq, ttk, rcfg, rrq, rcnt, s);
    CHECK(out.empty() && ttk.empty() && trq.size() == 1, "dropped frame: out %d ttk %d trq %d", (int)out.size(), (int)ttk.size(), (int)trq.size());
    while (!trq.empty()) trq.read();
    while (!out.empty()) out.read();
    want[C_FRAMES]++; want[a.second]++;
  }
  // counters through the request path
  req_t q; q.data = 1; rrq.write(q);
  resp_step(in, out, trq, ttk, rcfg, rrq, rcnt, s);
  CHECK(rcnt.size() == NCNT, "counter words %d", (int)rcnt.size());
  uint32_t got[NCNT];
  for (int i = 0; i < NCNT; i++) got[i] = rcnt.read().data;
  want[C_RES_LAST] = (uint32_t)(T3 - T2); want[C_RES_MAX] = want[C_RES_LAST];
  for (int i = 0; i < NCNT; i++) CHECK(got[i] == want[i], "counter %d = %u, want %u", i, got[i], want[i]);
  printf("ntp_resp: 2 replies byte-exact, %zu frames dropped into the right counters\n", all.size());

  // ntp_tod against a 96-bit model
  hls::stream<tcmd_t> tcfg("tcfg");
  hls::stream<tick_t> snap("snap"), tk("tk");
  hls::stream<req_t> rq("rq");
  tod_state t;
  tod_init(t);
  unsigned __int128 acc = 0, add = RATE_300, rate = RATE_300;
  const unsigned __int128 M96 = (((unsigned __int128)1) << 96) - 1;
  auto cmd = [&](unsigned op, uint64_t v) { tcmd_t c; c.data = 0; c.data.range(7, 0) = op; c.data.range(127, 64) = v; tcfg.write(c); };
  auto model = [&](int op, uint64_t v) {          // one cycle of the model, the same command as the kernel
    acc = (acc + add) & M96; unsigned __int128 nadd = rate;
    if (op == OP_RATE) { rate = v; nadd = v; }
    if (op == OP_STEP) nadd = (rate + ((unsigned __int128)(__int128)(int64_t)v << 32)) & M96;
    add = nadd;
  };
  const uint64_t step_back = (uint64_t)-(int64_t)(3ull << 32);   // -3 s
  for (int cyc = 0; cyc < 5000; cyc++) {
    int op = 0; uint64_t v = 0;
    if (cyc == 100) { op = OP_RATE; v = 61489146912ull + 61489; }        // +1 ppm
    if (cyc == 200) { op = OP_STEP; v = 5ull << 32; }                     // +5 s
    if (cyc == 300) { op = OP_STEP; v = step_back; }
    if (cyc == 400) { op = OP_SNAP; }
    if (op) cmd(op, v);
    if (cyc % 7 == 0) { req_t r; r.data = 0; rq.write(r); }
    tod_step(t, tcfg, snap, rq, tk);
    model(op == OP_SNAP ? 0 : op, v);
    const uint64_t m = (uint64_t)(acc >> 32);
    if (cyc % 7 == 0) { CHECK(!tk.empty(), "cycle %d: no timestamp", cyc); if (!tk.empty()) { uint64_t g = tk.read().data; CHECK(g == m, "cycle %d: tod %llx want %llx", cyc, (unsigned long long)g, (unsigned long long)m); } }
    if (cyc == 400) { CHECK(!snap.empty(), "no snapshot"); if (!snap.empty()) { uint64_t g = snap.read().data; CHECK(g == m, "snapshot %llx want %llx", (unsigned long long)g, (unsigned long long)m); } }
  }
  const uint64_t fin = (uint64_t)(acc >> 32);
  printf("ntp_tod: 5000 cycles = model (rate +1 ppm, +5 s, -3 s, snapshot); tod %u s + %u/2^32\n", (unsigned)(fin >> 32), (unsigned)fin);
  CHECK((fin >> 32) == 2, "tod seconds %u, want 2 (+5 -3)", (unsigned)(fin >> 32));
  CHECK(t.full == 0 && t.cmds == 4, "tod full %u cmds %u", t.full, t.cmds);
  // ntp_ctl: each command's requests, and the replies landing in out[]
  {
    hls::stream<tcmd_t> c_t("c_t"); hls::stream<tick_t> c_s("c_s"); hls::stream<rcmd_t> c_r("c_r");
    hls::stream<req_t> c_q("c_q"); hls::stream<cnt_t> c_c("c_c");
    uint32_t o[16];
    ntp_ctl(OP_RATE, 0x89abcdef, 0x12, o, c_t, c_s, c_r, c_q, c_c);
    CHECK(c_t.size() == 1 && c_t.read().data == ((ap_uint<128>(0x1289abcdefull) << 64) | OP_RATE), "ctl RATE");
    ntp_ctl(OP_SET, RF_STRATUM, 3, o, c_t, c_s, c_r, c_q, c_c);
    CHECK(c_r.size() == 1 && c_r.read().data == ((ap_uint<64>(3) << 32) | RF_STRATUM) && c_t.empty(), "ctl SET");
    tick_t w; w.data = 0x1122334455667788ull; c_s.write(w);
    ntp_ctl(OP_SNAP, 0, 0, o, c_t, c_s, c_r, c_q, c_c);
    CHECK(c_t.size() == 1 && (unsigned)c_t.read().data.range(7, 0) == OP_SNAP && o[0] == 0x55667788u && o[1] == 0x11223344u, "ctl SNAP");
    for (int i = 0; i < NCNT; i++) { cnt_t x; x.data = 100 + i; c_c.write(x); }
    w.data = (ap_uint<64>(4) << 32) | 2; c_s.write(w);
    ntp_ctl(OP_COUNT, 0, 0, o, c_t, c_s, c_r, c_q, c_c);
    bool ok = c_q.size() == 1 && c_t.size() == 1 && o[NCNT] == 2 && o[NCNT + 1] == 4;
    for (int i = 0; i < NCNT; i++) ok = ok && o[i] == 100u + i;
    CHECK(ok, "ctl COUNT");
    c_q.read(); c_t.read();
    printf("ntp_ctl: RATE / SET / SNAP / COUNT requests and replies as expected\n");
  }
  printf(fails ? "NTP-TB FAIL (%d)\n" : "NTP-TB PASS\n", fails);
  return fails ? 1 : 0;
}
