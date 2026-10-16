// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
// Checks zuc_model.h against the published ZUC, 128-EEA3 and 128-EIA3 test vectors (TS 35.223 test sets 1).
#include "zuc_model.h"
#include <cstdio>
int main()
{
  int bad = 0;
  if (int e = zuc_selftest()) { printf("ZUC keystream vector %d: FAIL\n", e); bad++; } else printf("ZUC keystream vectors 1,2: PASS\n");
  // EEA3 test set 1
  const uint8_t k1[16] = {0x17,0x3d,0x14,0xba,0x50,0x03,0x73,0x1d,0x7a,0x60,0x04,0x94,0x70,0xf0,0x0a,0x29};
  const uint8_t pt[28] = {0x6c,0xf6,0x53,0x40,0x73,0x55,0x52,0xab,0x0c,0x97,0x52,0xfa,0x6f,0x90,0x25,0xfe,
                          0x0b,0xd6,0x75,0xd9,0x00,0x58,0x75,0xb2,0x00,0x00,0x00,0x00};
  const uint8_t ct[25] = {0xa6,0xc8,0x5f,0xc6,0x6a,0xfb,0x85,0x33,0xaa,0xfc,0x25,0x18,0xdf,0xe7,0x84,0x94,
                          0x0e,0xe1,0xe4,0xb0,0x30,0x23,0x8c,0xc8,0x00};
  uint8_t iv[16], out[28];
  eea3_iv(0x66035492, 0xf, 0, iv); zuc_xor(k1, iv, pt, out, 28);
  out[24] &= 0x80;                                                  // 193 bits: only the top bit of byte 24 counts
  printf("EEA3 test set 1: %s\n", memcmp(out, ct, 25) ? (bad++, "FAIL") : "PASS");
  // EIA3 test set 1: key 0, count 0, bearer 0, direction 0, 1-bit message 0
  const uint8_t k0[16] = {0}; const uint8_t m0[4] = {0};
  eia3_iv(0, 0, 0, iv); const uint32_t mac = eia3_mac(k0, iv, m0, 1);
  printf("EIA3 test set 1: MAC 0x%08x %s\n", mac, mac == 0xc8a9595e ? "PASS" : (bad++, "FAIL"));
  return bad;
}
