// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// h4ml_jet_tb -- C test of h4ml_frame: every test vector (vectors.txt: 16 raw inputs, 5 raw expected outputs from hls4ml's
// own bit-accurate emulation, label) as a 94-byte frame; checks the 5 scores bit for bit, the winning class, the status
// byte, the header swap and that the rest of the frame is untouched. Also: a frame without the magic, a 1-beat frame
// and a 3-beat frame (echo only). Prints H4ML-TB PASS/FAIL. usage: h4ml_jet_tb vectors.txt [max]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ap_axi_sdata.h>
#include <hls_stream.h>
typedef ap_uint<512> beat_t;
typedef ap_axiu<512, 32, 0, 0> pkt_t;
void h4ml_frame(hls::stream<pkt_t> &in, hls::stream<pkt_t> &out);

static void put(beat_t *b, const unsigned char *f, int n) {
  for (int k = 0; k < n; k++) b[k / 64].range(8 * (k % 64) + 7, 8 * (k % 64)) = f[k];
}
static void get(const beat_t *b, unsigned char *f, int n) {
  for (int k = 0; k < n; k++) f[k] = (unsigned)b[k / 64].range(8 * (k % 64) + 7, 8 * (k % 64));
}
static int run(const unsigned char *f, int n, unsigned char *o) {   // returns the number of beats out
  int nb = (n + 63) / 64; beat_t b[4] = {0, 0, 0, 0}; put(b, f, n);
  hls::stream<pkt_t> in, out;
  for (int i = 0; i < nb; i++) { pkt_t p; p.data = b[i]; p.keep = -1; p.strb = -1; p.user = 0x51cb; p.last = i == nb - 1; in.write(p); }
  h4ml_frame(in, out);
  int k = 0; beat_t r[4];
  while (!out.empty()) { pkt_t p = out.read(); r[k] = p.data; if (p.user != 0x51cb) return -1; k++; }
  get(r, o, k * 64); return k;
}
static void hdr(unsigned char *f) {
  for (int i = 0; i < 42; i++) f[i] = 0x10 + i;
  f[12] = 0x08; f[13] = 0x00;
}
static bool swapped(const unsigned char *f, const unsigned char *o) {
  for (int i = 0; i < 6; i++) if (o[i] != f[6 + i] || o[6 + i] != f[i]) return false;
  for (int i = 0; i < 4; i++) if (o[26 + i] != f[30 + i] || o[30 + i] != f[26 + i]) return false;
  for (int i = 0; i < 2; i++) if (o[34 + i] != f[36 + i] || o[36 + i] != f[34 + i]) return false;
  return o[24] == 0 && o[25] == 0 && o[40] == 0 && o[41] == 0;
}
int main(int argc, char **argv) {
  FILE *v = fopen(argc > 1 ? argv[1] : "vectors.txt", "r"); if (!v) { printf("H4ML-TB FAIL no vectors\n"); return 1; }
  long max = argc > 2 ? atol(argv[2]) : -1, n = 0, bad = 0, agree = 0;
  int x[16], y[5], lab;
  while ((max < 0 || n < max) && fscanf(v, "%d", &x[0]) == 1) {
    for (int i = 1; i < 16; i++) fscanf(v, "%d", &x[i]);
    for (int c = 0; c < 5; c++) fscanf(v, "%d", &y[c]);
    fscanf(v, "%d", &lab);
    unsigned char f[256] = {0}, o[256];
    hdr(f); memcpy(f + 42, "JET1", 4); f[46] = n & 255; f[47] = (n >> 8) & 255; f[48] = (n >> 16) & 255; f[49] = 0x5a;
    for (int i = 0; i < 16; i++) { f[50 + 2 * i] = x[i] & 255; f[51 + 2 * i] = (x[i] >> 8) & 255; }
    if (run(f, 94, o) != 2 || !swapped(f, o)) { bad++; continue; }
    int best = 0; bool ok = true;
    for (int c = 0; c < 5; c++) { short r = o[82 + 2 * c] | (o[83 + 2 * c] << 8); if (r != (short)y[c]) ok = false; if (y[c] > y[best]) best = c; }
    if (o[92] != best || o[93] != 1) ok = false;
    for (int k = 42; k < 82; k++) if (o[k] != f[k]) ok = false;
    for (int k = 94; k < 128; k++) if (o[k] != 0) ok = false;
    if (!ok && bad < 5) printf("mismatch at vector %ld\n", n);
    bad += !ok; agree += o[92] == lab; n++;
  }
  // a frame without the magic, a 1-beat frame, a 3-beat frame: echo only
  int ebad = 0;
  { unsigned char f[256] = {0}, o[256]; hdr(f); memcpy(f + 42, "JETX", 4); for (int k = 46; k < 94; k++) f[k] = k;
    if (run(f, 94, o) != 2 || !swapped(f, o) || memcmp(f + 42, o + 42, 52)) ebad++; }
  { unsigned char f[256] = {0}, o[256]; hdr(f); for (int k = 42; k < 60; k++) f[k] = k;
    if (run(f, 60, o) != 1 || !swapped(f, o) || memcmp(f + 42, o + 42, 18)) ebad++; }
  { unsigned char f[256] = {0}, o[256]; hdr(f); memcpy(f + 42, "JET1", 4); for (int k = 46; k < 180; k++) f[k] = k * 7;
    if (run(f, 180, o) != 3 || !swapped(f, o) || o[93] != 1 || memcmp(f + 94, o + 94, 86)) ebad++; }
  printf("H4ML-TB %s: %ld vectors, %ld mismatching, %d echo-case failures; accuracy vs labels %.4f\n",
         bad || ebad || !n ? "FAIL" : "PASS", n, bad, ebad, n ? (double)agree / n : 0.0);
  return bad || ebad || !n;
}
