// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// csim for the mcd kernel: replays the Xilinx regression (regressionSims ALLSEQS, converted to the 'W n' / no-'D'
// format) as UDP datagrams -- meta_t + 512-bit beats, as udp_rx_lite delivers them -- through rx_adapt, the
// pipeline, tx_adapt and the two DDR memory servers (arrays behind the m_axi pointers), then turns the responses
// back into the regression's 64-bit word format and compares with the golden file byte for byte.
// Also checks every response's metadata: length == bytes delivered, client IP/port and local port swapped back.
//   usage: mcd_kernel_tb <in.txt> <golden3.txt>      MUTATE=1: flips one golden byte -> must FAIL
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

struct req { std::vector<unsigned long long> w; std::vector<unsigned> k; unsigned wait; };

int main(int argc, char **argv)
{
  if (argc != 3) { printf("usage: %s in.txt golden3.txt\n", argv[0]); return 2; }
  std::ifstream in(argv[1]), gold(argv[2]);
  std::vector<req> reqs; req cur{{}, {}, 0}; unsigned pending_wait = 0; std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream ss(line); std::string a, b, c; ss >> a >> b >> c;
    if (a == "W") { pending_wait += atoi(b.c_str()); continue; }
    if (cur.w.empty()) { cur.wait = pending_wait; pending_wait = 0; }
    cur.w.push_back(strtoull(a.c_str(), nullptr, 16)); cur.k.push_back(strtoul(b.c_str(), nullptr, 16));
    if (atoi(c.c_str()) == 1) { reqs.push_back(cur); cur = req{{}, {}, 0}; }
  }
  std::vector<std::string> golden; while (std::getline(gold, line)) if (!line.empty()) golden.push_back(line);
  if (getenv("MUTATE") && golden.size() > 100) golden[100][0] = golden[100][0] == '0' ? '1' : '0';

  std::vector<ap_uint<512> > ht(1024), vs(32768u * 32u);
  hls::stream<meta_t> rxm("rxm"), txm("txm"); hls::stream<nax_t> rxd("rxd"), txd("txd");
  cs_t hrc("hrc"), hwc("hwc"), vrc("vrc"), vwc("vwc"), dbg1("dbg1"), dbg2("dbg2"); unsigned long long dbgv[64] = {0}; ds_t hrd("hrd"), hwd("hwd"), vrd("vrd"), vwd("vwd");
  // mcd_mem is started late (host order: load xclbin, allocate buffers, start mcd_mem): the hash table's start-up
  // zeroing must wait in the streams and still land in the right memory
  const long MEMSTART = getenv("MEMSTART") ? atol(getenv("MEMSTART")) : 5000;
  const unsigned IP = 0x0A01A8C0, CPORT = 40001, LPORT = 11211;   // 192.168.1.10 (wire order), client/local ports
  std::vector<std::string> out; unsigned meta_bad = 0, nresp = 0; size_t r = 0; unsigned idle = 0;
  std::vector<unsigned long long> curw; std::vector<unsigned> curk; unsigned curlen = 0, curbytes = 0;
  for (long cyc = 0; cyc < 4000000; cyc++) {
    if (r < reqs.size() && rxd.empty() && cyc >= MEMSTART) {
      if (idle < reqs[r].wait) idle++;
      else {
        idle = 0; const req &q = reqs[r++];
        unsigned bytes = 0; for (unsigned k : q.k) bytes += __builtin_popcount(k);
        meta_t m = 0; m.range(127, 96) = IP; m.range(143, 128) = CPORT; m.range(159, 144) = LPORT; m.range(175, 160) = bytes;
        rxm.write(m);
        for (size_t i = 0; i < q.w.size(); i += 8) {
          nax_t b; b.data = 0; b.keep = 0;
          for (size_t j = 0; j < 8 && i + j < q.w.size(); j++) {
            b.data.range(64 * j + 63, 64 * j) = q.w[i + j]; b.keep.range(8 * j + 7, 8 * j) = q.k[i + j]; }
          b.last = i + 8 >= q.w.size(); rxd.write(b);
        }
      }
    }
    mcd(rxm, rxd, txm, txd, hrc, hrd, hwc, hwd, vrc, vrd, vwc, vwd, dbg1);
    if (cyc >= MEMSTART) mcd_mem_tb_step(hrc, hrd, hwc, hwd, vrc, vrd, vwc, vwd, ht.data(), vs.data(), dbg2);
    while (!dbg1.empty()) { ap_uint<64> w = dbg1.read(); dbgv[(unsigned)w.range(61, 56)] = (unsigned long long)w.range(55, 0); }
    while (!dbg2.empty()) { ap_uint<64> w = dbg2.read(); dbgv[(unsigned)w.range(61, 56)] = (unsigned long long)w.range(55, 0); }
    if (getenv("MAXCYC") && cyc >= atol(getenv("MAXCYC"))) break;
    if (getenv("PROGRESS") && cyc % 200000 == 0) fprintf(stderr, "cyc %ld: sent %zu/%zu requests, %u responses, %zu/%zu lines\n", cyc, r, reqs.size(), nresp, out.size(), golden.size());
    meta_t tm;
    if (!txm.empty() && curw.empty() && curlen == 0) {
      tm = txm.read(); nresp++; curlen = (unsigned)tm.range(175, 160);
      if ((unsigned)tm.range(127, 96) != IP || (unsigned)tm.range(143, 128) != CPORT || (unsigned)tm.range(159, 144) != LPORT) {
        if (meta_bad++ < 3) printf("meta %u: ip %08x cport %u lport %u\n", nresp, (unsigned)tm.range(127, 96), (unsigned)tm.range(143, 128), (unsigned)tm.range(159, 144)); }
    }
    while (!txd.empty()) {
      nax_t b = txd.read();
      for (int j = 0; j < 8; j++) {
        const unsigned kk = (unsigned)b.keep.range(8 * j + 7, 8 * j);
        if (!kk) break;
        curw.push_back((unsigned long long)b.data.range(64 * j + 63, 64 * j)); curk.push_back(kk); curbytes += __builtin_popcount(kk);
      }
      if (b.last) {
        if (curbytes != curlen) { if (meta_bad++ < 6) printf("response %u: length %u in meta, %u bytes delivered\n", nresp, curlen, curbytes); }
        for (size_t i = 0; i < curw.size(); i++) {
          char s[64]; snprintf(s, sizeof s, "%016llX %02X %d", curw[i], curk[i], i + 1 == curw.size() ? 1 : 0); out.push_back(s); }
        curw.clear(); curk.clear(); curlen = 0; curbytes = 0;
      }
    }
    if (r == reqs.size() && rxd.empty() && out.size() >= golden.size() && cyc > 0) { if (++idle > 20000) break; }
  }
  printf("debug counters (last snapshot):"); for (int k = 1; k < 25; k++) if (dbgv[k]) printf(" %d=%llu", k, dbgv[k]); printf("\n");
  size_t diff = 0, first = (size_t)-1;
  for (size_t i = 0; i < golden.size() && i < out.size(); i++) if (golden[i] != out[i]) { if (!diff++) first = i; }
  printf("mcd kernel csim: %zu requests, %u responses, %zu/%zu output lines, %zu differ from golden%s, %u metadata errors\n",
         reqs.size(), nresp, out.size(), golden.size(), diff, first != (size_t)-1 ? (" (first at line " + std::to_string(first + 1) + ")").c_str() : "", meta_bad);
  if (first != (size_t)-1) printf("  got  %s\n  want %s\n", out[first].c_str(), golden[first].c_str());
  const bool ok = diff == 0 && out.size() == golden.size() && meta_bad == 0;
  printf("mcd_kernel csim: %s%s\n", ok ? "PASS" : "FAIL", getenv("MUTATE") ? " [MUTATE: FAIL expected]" : "");
  return !ok;
}
