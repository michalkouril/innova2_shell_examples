// E5.2 unit test of zuc_in / zuc_out (software, threads): byte order, guard, echo filter.
#include "zuc_adapt.cpp"
#include <thread>
#include <cstdio>
#include <cstring>
#include <chrono>
static void send(hls::stream<pkt_t> &s, const uint8_t *f, unsigned len)
{
  const unsigned nb = (len + 63) / 64;
  for (unsigned b = 0; b < nb; b++) {
    pkt_t p; beat_t d = 0; ap_uint<64> k = 0;
    for (unsigned i = 0; i < 64 && 64 * b + i < len; i++) { d.range(8 * i + 7, 8 * i) = f[64 * b + i]; k[i] = 1; }
    p.data = d; p.keep = k; p.strb = k; p.user = len; p.last = b == nb - 1; s.write(p);
  }
}
static unsigned req(uint8_t *f, unsigned op, unsigned L, unsigned frame_len)
{
  memset(f, 0, 2048); for (unsigned i = 0; i < frame_len; i++) f[i] = i * 7 + 1;
  f[24] = 0x12; f[25] = 0x34; f[64] = op; f[65] = 0x5A; f[66] = L >> 8; f[67] = L; return frame_len;
}
int main()
{
  hls::stream<pkt_t> a("a"), d("d"); hls::stream<afu_t> b("b"), c("c");
  std::thread t1([&] { zuc_in(a, b); }); t1.detach();
  std::thread t2([&] { zuc_out(c, d); }); t2.detach();
  uint8_t f[2048]; int bad = 0;
  struct { unsigned op, L, flen; bool pass; const char *name; } T[] = {
    {0, 100, 228, true, "CONF 100 ok"}, {1, 13, 141, true, "INTEG 13 ok"}, {0, 0, 128, false, "len 0"},
    {0, 100, 64, false, "runt"}, {4, 64, 192, false, "opcode 4"}, {0, 300, 192, false, "beat mismatch"},
    {1, 64, 192, true, "INTEG 64 ok"}, {2, 64, 192, false, "opcode 2 (bypass)"}};
  unsigned expect = 0;
  for (auto &t : T) { send(a, f, req(f, t.op, t.L, t.flen)); expect += t.pass; }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  unsigned got = 0;
  afu_t x;
  while (b.read_nb(x)) { got += x.last; if (x.user != 0) { printf("tuser not 0\n"); bad++; } }
  printf("zuc_in forwarded %u requests, expected %u: %s\n", got, expect, got == expect ? "PASS" : (bad++, "FAIL"));
  // zuc_out: one processed reply (IP checksum 0) and one bypass echo (checksum 0x1234), AFU byte order, 2 beats + 5
  for (int e = 0; e < 2; e++) {
    for (int bt = 0; bt < 2; bt++) {
      afu_t r; beat_t v = 0;
      for (int i = 0; i < 64; i++) v.range(8 * (63 - i) + 7, 8 * (63 - i)) = (bt * 64 + i) & 0xff;   // byte i at MSB end
      if (bt == 0) { v.range(8 * (63 - 24) + 7, 8 * (63 - 24)) = e ? 0x12 : 0; v.range(8 * (63 - 25) + 7, 8 * (63 - 25)) = e ? 0x34 : 0; }
      r.data = v; r.keep = bt ? ap_uint<64>(0xF800000000000000ull) : ap_uint<64>(-1); r.last = bt == 1; r.user = 0; c.write(r);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  unsigned frames = 0, len = 0; bool order = true; pkt_t p;
  while (d.read_nb(p)) { if (p.last) frames++; len = p.user; if ((unsigned)p.data.range(7, 0) != 0 && !p.last) order = false; }
  printf("zuc_out forwarded %u frames (expect 1), length %u (expect 69), byte order %s\n", frames, len, order ? "ok" : "WRONG");
  if (frames != 1 || len != 69 || !order) bad++;
  printf("ADAPT %s\n", bad ? "FAIL" : "PASS"); fflush(stdout); _Exit(bad);
}
