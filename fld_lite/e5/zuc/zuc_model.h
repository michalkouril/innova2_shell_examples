// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.2 software reference for the ZUC accelerator: ZUC-128 (ETSI/SAGE spec v1.6 / 3GPP TS 35.221), plus the 128-EEA3
// and 128-EIA3 constructions (TS 35.222). Header-only, used by the host to check every accelerator response.
// Self-test: zuc_selftest() checks the spec's keystream test vectors (all-zero and all-0xFF key/IV).
#pragma once
#include <stdint.h>
#include <string.h>
#include "zuc_sbox.h"

struct zuc_state { uint32_t s[16], r1, r2, x0, x1, x2, x3; };

static inline uint32_t zuc_add31(uint32_t a, uint32_t b) { uint32_t c = a + b; return (c & 0x7fffffff) + (c >> 31); }
static inline uint32_t zuc_rot31(uint32_t a, int k) { return ((a << k) | (a >> (31 - k))) & 0x7fffffff; }
static inline uint32_t zuc_rot32(uint32_t a, int k) { return (a << k) | (a >> (32 - k)); }
static inline uint32_t zuc_l1(uint32_t x) { return x ^ zuc_rot32(x, 2) ^ zuc_rot32(x, 10) ^ zuc_rot32(x, 18) ^ zuc_rot32(x, 24); }
static inline uint32_t zuc_l2(uint32_t x) { return x ^ zuc_rot32(x, 8) ^ zuc_rot32(x, 14) ^ zuc_rot32(x, 22) ^ zuc_rot32(x, 30); }
static inline uint32_t zuc_sbox(uint32_t x)
{
  return ((uint32_t)ZUC_S0[x >> 24] << 24) | ((uint32_t)ZUC_S1[(x >> 16) & 0xff] << 16) |
         ((uint32_t)ZUC_S0[(x >> 8) & 0xff] << 8) | ZUC_S1[x & 0xff];
}
static inline void zuc_br(zuc_state *z)
{
  z->x0 = ((z->s[15] & 0x7fff8000) << 1) | (z->s[14] & 0xffff);
  z->x1 = ((z->s[11] & 0xffff) << 16) | (z->s[9] >> 15);
  z->x2 = ((z->s[7] & 0xffff) << 16) | (z->s[5] >> 15);
  z->x3 = ((z->s[2] & 0xffff) << 16) | (z->s[0] >> 15);
}
static inline uint32_t zuc_f(zuc_state *z)
{
  const uint32_t w = (z->x0 ^ z->r1) + z->r2, w1 = z->r1 + z->x1, w2 = z->r2 ^ z->x2;
  const uint32_t u = zuc_l1((w1 << 16) | (w2 >> 16)), v = zuc_l2((w2 << 16) | (w1 >> 16));
  z->r1 = zuc_sbox(u); z->r2 = zuc_sbox(v);
  return w;
}
static inline void zuc_lfsr(zuc_state *z, uint32_t u)                // u = 0 in working mode
{
  uint32_t f = z->s[0];
  f = zuc_add31(f, zuc_rot31(z->s[0], 8));
  f = zuc_add31(f, zuc_rot31(z->s[4], 20));
  f = zuc_add31(f, zuc_rot31(z->s[10], 21));
  f = zuc_add31(f, zuc_rot31(z->s[13], 17));
  f = zuc_add31(f, zuc_rot31(z->s[15], 15));
  if (u) f = zuc_add31(f, u);
  if (f == 0) f = 0x7fffffff;
  memmove(z->s, z->s + 1, 15 * sizeof(uint32_t));
  z->s[15] = f;
}
static inline void zuc_init(zuc_state *z, const uint8_t key[16], const uint8_t iv[16])
{
  static const uint32_t D[16] = {0x44D7, 0x26BC, 0x626B, 0x135E, 0x5789, 0x35E2, 0x7135, 0x09AF,
                                 0x4D78, 0x2F13, 0x6BC4, 0x1AF1, 0x5E26, 0x3C4D, 0x789A, 0x47AC};
  for (int i = 0; i < 16; i++) z->s[i] = ((uint32_t)key[i] << 23) | (D[i] << 8) | iv[i];
  z->r1 = z->r2 = 0;
  for (int i = 0; i < 32; i++) { zuc_br(z); const uint32_t w = zuc_f(z); zuc_lfsr(z, w >> 1); }
  zuc_br(z); zuc_f(z); zuc_lfsr(z, 0);                                 // first working-mode step, output discarded
}
static inline uint32_t zuc_word(zuc_state *z) { zuc_br(z); const uint32_t k = zuc_f(z) ^ z->x3; zuc_lfsr(z, 0); return k; }
static inline void zuc_keystream(const uint8_t key[16], const uint8_t iv[16], uint32_t *ks, unsigned n)
{
  zuc_state z; zuc_init(&z, key, iv);
  for (unsigned i = 0; i < n; i++) ks[i] = zuc_word(&z);
}

// 128-EEA3 / 128-EIA3 IVs from COUNT, BEARER, DIRECTION (TS 35.222 section 3)
static inline void eea3_iv(uint32_t count, uint32_t bearer, uint32_t dir, uint8_t iv[16])
{
  iv[0] = count >> 24; iv[1] = count >> 16; iv[2] = count >> 8; iv[3] = count;
  iv[4] = ((bearer << 3) | ((dir & 1) << 2)) & 0xfc; iv[5] = iv[6] = iv[7] = 0;
  for (int i = 0; i < 8; i++) iv[8 + i] = iv[i];
}
static inline void eia3_iv(uint32_t count, uint32_t bearer, uint32_t dir, uint8_t iv[16])
{
  iv[0] = count >> 24; iv[1] = count >> 16; iv[2] = count >> 8; iv[3] = count;
  iv[4] = (bearer << 3) & 0xf8; iv[5] = iv[6] = iv[7] = 0;
  for (int i = 0; i < 8; i++) iv[8 + i] = iv[i];
  iv[8] ^= (dir & 1) << 7; iv[14] ^= (dir & 1) << 7;
}
// XOR `len` bytes with the keystream (keystream word i covers bytes 4i..4i+3, most significant byte first)
static inline void zuc_xor(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, uint8_t *out, unsigned len)
{
  zuc_state z; zuc_init(&z, key, iv);
  uint32_t w = 0;
  for (unsigned i = 0; i < len; i++) {
    if ((i & 3) == 0) w = zuc_word(&z);
    out[i] = in[i] ^ (uint8_t)(w >> (24 - 8 * (i & 3)));
  }
}
// 128-EIA3 MAC over `bits` message bits (message bytes MSB-first), TS 35.222 section 4
static inline uint32_t eia3_mac(const uint8_t key[16], const uint8_t iv[16], const uint8_t *msg, uint32_t bits)
{
  const unsigned L = (bits + 31) / 32 + 2;
  uint32_t ks[L + 1];
  zuc_keystream(key, iv, ks, L);
  auto kw = [&](uint32_t i) -> uint32_t {                            // the 32-bit keystream word starting at bit i
    const uint32_t j = i / 32, o = i % 32;
    return o ? (ks[j] << o) | (ks[j + 1] >> (32 - o)) : ks[j];
  };
  uint32_t t = 0;
  for (uint32_t i = 0; i < bits; i++)
    if (msg[i / 8] & (0x80 >> (i % 8))) t ^= kw(i);
  t ^= kw(bits);
  return t ^ kw(32 * (L - 1));
}
static inline int zuc_selftest(void)
{
  uint8_t k[16], v[16]; uint32_t ks[2];
  memset(k, 0, 16); memset(v, 0, 16); zuc_keystream(k, v, ks, 2);
  if (ks[0] != 0x27bede74 || ks[1] != 0x018082da) return 1;
  memset(k, 0xff, 16); memset(v, 0xff, 16); zuc_keystream(k, v, ks, 2);
  if (ks[0] != 0x0657cfa0 || ks[1] != 0x7096398b) return 2;
  return 0;
}
