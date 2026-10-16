// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
// C simulation of rsa_modexp against make_vectors.py's expected results.
#include <ap_int.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
typedef ap_uint<512> beat;
extern "C" void rsa_modexp(const beat *in, beat *out, unsigned njobs);
static ap_uint<1024> hex(const std::string &s) {
  ap_uint<1024> v = 0;
  for (char c : s) { v <<= 4; v |= (c <= '9' ? c - '0' : c - 'a' + 10); }
  return v;
}
int main(int argc, char **argv) {
  std::ifstream f(argc > 1 ? argv[1] : "vectors.txt"); std::string line;
  std::vector<beat> in; std::vector<ap_uint<1024> > want;
  while (std::getline(f, line)) {
    std::istringstream ss(line); std::string w[6];
    for (auto &x : w) ss >> x;
    for (int k = 0; k < 5; k++) { ap_uint<1024> v = hex(w[k]); in.push_back(v.range(511, 0)); in.push_back(v.range(1023, 512)); }
    want.push_back(hex(w[5]));
  }
  const unsigned n = want.size(); std::vector<beat> out(2 * n);
  rsa_modexp(in.data(), out.data(), n);
  int bad = 0;
  for (unsigned k = 0; k < n; k++) {
    ap_uint<1024> g; g.range(511, 0) = out[2 * k]; g.range(1023, 512) = out[2 * k + 1];
    if (g != want[k]) { bad++; printf("job %u MISMATCH\n", k); }
  }
  printf("RSA-TB %u jobs, %d mismatches\n", n, bad);
  return bad != 0;
}
