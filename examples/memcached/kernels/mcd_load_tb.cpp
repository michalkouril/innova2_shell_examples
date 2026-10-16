// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// csim for the mcd kernel under load-like traffic (mcd_kernel_tb.cpp replays the Xilinx regression; this one checks
// what it does not cover). Requests go in as UDP datagrams (meta_t + 512-bit beats, as udp_rx_lite delivers them)
// through mcd and the two memory servers, as mcd_load sends them (keys k%08u, binary protocol, no frame header):
//   1. preload: SET each of the 256 keys once, one at a time       -> 249 ok, 7 refused (their bucket is full; the
//                                                                     card reports the same 249 / 7)
//   2. overwrite: SET every key again with a new 40 B value, then GET every key -> each stored key returns the NEW
//      value; overwriting a key whose bucket had no free bin hung the pipeline until memWrite.cpp's 2026-10-17 fix
//   3. mixes: 20000 requests with 64 in flight, 10 % and 100 % SETs on random keys -> every request answered
// Any request without a response for 2M cycles is a hang (FAIL).
//   build: g++ -O1 -std=c++14 -DDBG_TICK_BITS=8 -I<Vitis_HLS>/include -Ipipeline -o mcd_load_tb mcd_load_tb.cpp
//          mcd_kernel.cpp pipeline/{globals,merger,splitter}.cpp pipeline/hashTable/{cc,compare,hash,hashTable,
//          memRead,memWrite}.cpp pipeline/requestParser/requestParser.cpp pipeline/responseFormatter/binResponse.cpp
//          pipeline/valueStore/{valueStore,flashValueStore}.cpp          (runs in about a second; with the original
//          memWrite.cpp it hangs on the overwrite of k00000003)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "globals.h"
#include <ap_axi_sdata.h>
typedef ap_axiu<512, 0, 0, 0> nax_t;
typedef ap_uint<176> meta_t;
typedef hls::stream<ap_uint<64> > cs_t; typedef hls::stream<ap_uint<512> > ds_t;
extern "C" void mcd(hls::stream<meta_t> &, hls::stream<nax_t> &, hls::stream<meta_t> &, hls::stream<nax_t> &,
                    cs_t &, ds_t &, cs_t &, ds_t &, cs_t &, ds_t &, cs_t &, ds_t &, cs_t &);
void mcd_mem_tb_step(cs_t &, ds_t &, cs_t &, ds_t &, cs_t &, ds_t &, cs_t &, ds_t &, ap_uint<512> *, ap_uint<512> *, cs_t &);

static std::vector<ap_uint<512> > ht(1024), vsm(32768u * 32u);
static hls::stream<meta_t> rxm("rxm"), txm("txm"); static hls::stream<nax_t> rxd("rxd"), txd("txd");
static cs_t hrc("hrc"), hwc("hwc"), vrc("vrc"), vwc("vwc"), dbg1("dbg1"), dbg2("dbg2");
static ds_t hrd("hrd"), hwd("hwd"), vrd("vrd"), vwd("vwd");
static std::vector<uint8_t> resp;

static uint8_t vbyte(const std::string &k, int i, int gen) { return (uint8_t)(i * (gen ? 13 : 7) + 1 + (gen ? k[8] : 0)); }
static std::vector<uint8_t> set_req(const std::string &k, int vs, int gen) {
  std::vector<uint8_t> p(24 + 8 + k.size() + vs, 0);
  p[0] = 0x80; p[1] = 0x01; p[3] = (uint8_t)k.size(); p[4] = 8; p[11] = (uint8_t)(8 + k.size() + vs);
  p[24] = 0xDE; p[25] = 0xAD; p[26] = 0xBE; p[27] = 0xEF;
  memcpy(&p[32], k.data(), k.size());
  for (int i = 0; i < vs; i++) p[32 + k.size() + i] = vbyte(k, i, gen);
  return p;
}
static std::vector<uint8_t> get_req(const std::string &k) {
  std::vector<uint8_t> p(24 + k.size(), 0);
  p[0] = 0x80; p[3] = (uint8_t)k.size(); p[11] = (uint8_t)k.size(); memcpy(&p[24], k.data(), k.size());
  return p;
}
static void send(const std::vector<uint8_t> &p) {
  meta_t m = 0; m.range(127, 96) = 0x0A01A8C0; m.range(143, 128) = 40001; m.range(159, 144) = 11211; m.range(175, 160) = p.size();
  rxm.write(m);
  for (size_t i = 0; i < p.size(); i += 64) {
    nax_t b; b.data = 0; b.keep = 0;
    for (size_t j = 0; j < 64 && i + j < p.size(); j++) { b.data.range(8 * j + 7, 8 * j) = p[i + j]; b.keep[j] = 1; }
    b.last = i + 64 >= p.size(); rxd.write(b);
  }
}
// one cycle of the kernel and the memory servers; returns true when a whole response is in resp
static bool step() {
  mcd(rxm, rxd, txm, txd, hrc, hrd, hwc, hwd, vrc, vrd, vwc, vwd, dbg1);
  mcd_mem_tb_step(hrc, hrd, hwc, hwd, vrc, vrd, vwc, vwd, ht.data(), vsm.data(), dbg2);
  while (!dbg1.empty()) dbg1.read();
  while (!dbg2.empty()) dbg2.read();
  if (!txm.empty()) txm.read();
  bool done = false;
  while (!txd.empty() && !done) {
    nax_t b = txd.read();
    for (int j = 0; j < 64; j++) if (b.keep[j]) resp.push_back((uint8_t)b.data.range(8 * j + 7, 8 * j));
    done = b.last;
  }
  return done;
}
static bool one(const std::vector<uint8_t> &p) {   // send one request, wait for its response (in resp)
  resp.clear(); send(p);
  for (long c = 0; c < 2000000; c++) if (step()) return true;
  return false;
}
static int status() { return resp.size() >= 8 ? (resp[6] << 8 | resp[7]) : -1; }

int main() {
  for (int c = 0; c < 20000; c++) step();          // the hash table's start-up zeroing
  std::vector<std::string> keys; char b[16];
  for (int i = 0; i < 256; i++) { snprintf(b, 16, "k%08u", i); keys.push_back(b); }
  bool pass = true; int ok = 0, refused = 0;
  for (auto &k : keys) {
    if (!one(set_req(k, 32, 0))) { printf("HANG on preload SET %s\n", k.c_str()); return 1; }
    status() == 0 ? ok++ : refused++;
  }
  printf("preload: 256 SETs, ok %d, refused %d (bucket full)%s\n", ok, refused, ok == 249 ? "" : "  <-- expected 249");
  pass &= ok == 249;
  int setok = 0, good = 0, wrong = 0, miss = 0;
  for (auto &k : keys) {
    if (!one(set_req(k, 40, 1))) { printf("HANG on overwrite SET %s\n", k.c_str()); return 1; }
    setok += status() == 0;
  }
  for (auto &k : keys) {
    if (!one(get_req(k))) { printf("HANG on GET %s\n", k.c_str()); return 1; }
    if (status() != 0) { miss++; continue; }
    const int el = resp[4], kl = resp[2] << 8 | resp[3]; bool same = resp.size() == 24 + (size_t)el + kl + 40;
    for (int i = 0; same && i < 40; i++) same = resp[24 + el + kl + i] == vbyte(k, i, 1);
    same ? good++ : wrong++;
  }
  printf("overwrite: 256 SETs with new 40 B values, ok %d; GET: new value %d, wrong %d, miss %d\n", setok, good, wrong, miss);
  pass &= setok == ok && good == ok && wrong == 0;
  for (int setpct : {10, 100}) {
    const long total = 20000; const int W = 64; long sent = 0, got = 0, last = 0, c = 0; uint32_t rng = 12345;
    resp.clear();
    while (got < total) {
      if (sent - got < W && sent < total && rxd.size() < 4) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const std::string &k = keys[rng % keys.size()];
        send((int)((rng >> 8) % 100) < setpct ? set_req(k, 32, 0) : get_req(k)); sent++;
      }
      if (step()) { resp.clear(); got++; last = c; }
      if (++c - last > 2000000) { printf("HANG in the %d %% SET mix: sent %ld, answered %ld\n", setpct, sent, got); return 1; }
    }
    printf("mix: %ld requests, %d in flight, %d %% SETs: all answered\n", total, W, setpct);
  }
  printf("mcd_load_tb: %s\n", pass ? "PASS" : "FAIL");
  return !pass;
}
