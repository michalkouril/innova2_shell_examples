// csim check for udp_swap: one 2-beat frame, every swapped/zeroed byte and the untouched payload
#include <cstdio>
#include <hls_stream.h>
#include <ap_axi_sdata.h>
typedef ap_uint<512> beat_t; typedef ap_axiu<512, 32, 0, 0> pkt_t;
void udp_swap_step(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out, bool &first);
int main()
{
  unsigned char f[100]; for (int i = 0; i < 100; i++) f[i] = (unsigned char)(i * 3 + 1);
  hls::stream<pkt_t> a, b;
  for (int q = 0; q < 2; q++) { pkt_t p; beat_t d = 0; for (int i = 0; i < 64 && q * 64 + i < 100; i++) d.range(8 * i + 7, 8 * i) = f[q * 64 + i];
    p.data = d; p.keep = q ? ap_uint<64>((1ull << 36) - 1) : ap_uint<64>(-1); p.last = q == 1; p.user = 100; a.write(p); }
  // the kernel is free-running (while(1)): csim drives its per-beat step directly, two beats = one frame
  unsigned char g[128] = {0};
  // emulate two iterations of the loop body
  bool first = true; for (int k = 0; k < 2; k++) udp_swap_step(a, b, first);
  for (int q = 0; q < 2; q++) { pkt_t p = b.read(); for (int i = 0; i < 64; i++) g[q * 64 + i] = p.data.range(8 * i + 7, 8 * i); }
  int bad = 0;
  for (int i = 0; i < 100; i++) {
    int e = f[i];
    if (i < 6) e = f[6 + i]; else if (i < 12) e = f[i - 6];
    else if (i >= 26 && i < 30) e = f[i + 4]; else if (i >= 30 && i < 34) e = f[i - 4];
    else if (i >= 34 && i < 36) e = f[i + 2]; else if (i >= 36 && i < 38) e = f[i - 2];
    if (i == 24 || i == 25 || i == 40 || i == 41) e = 0;
    if (g[i] != e) { bad++; printf("byte %d: %02x expected %02x\n", i, g[i], e); }
  }
  printf("udp_swap csim: %s (%d bad bytes)\n", bad ? "FAIL" : "PASS", bad); return bad != 0;
}
