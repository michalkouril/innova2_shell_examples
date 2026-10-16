// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// hostmem_probe (tier-1 host memory access experiment): an RP kernel reading/writing HOST RAM through a peer_window
// window (sp=...:peer). `mem` points at the PEER aperture; `off` (bytes) selects window w at w * 1 MB.
//   mode 0: write n 32-byte beats from off, word k of beat i = pattern ^ ((8*i + k) * 0x9E3779B9)   (burst writes)
//   mode 1: read n beats from off, XOR-fold them into res[0..7]                                  (burst reads)
//   mode 2: n DEPENDENT single-beat reads (the next address depends on the data) -- read latency
//   mode 3: n iterations of the mode-2 loop without the read -- loop overhead
// res[8] = n done. Host computes the expected values (hostmem_host.cpp).
#include <ap_int.h>
#include <stdint.h>
typedef ap_uint<256> beat_t;

extern "C" void hostmem_probe(beat_t *mem, uint32_t *res, unsigned off, unsigned n, unsigned mode, unsigned pattern)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=32768 max_read_burst_length=64 max_write_burst_length=64 num_read_outstanding=16 num_write_outstanding=16 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem1 depth=16
  const unsigned b0 = off / 32;
  beat_t acc = 0;
  if (mode == 0) {
  wr:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32768
      beat_t v;
      for (int k = 0; k < 8; k++) v.range(32 * k + 31, 32 * k) = pattern ^ ((8 * i + k) * 0x9E3779B9u);
      mem[b0 + i] = v;
    }
  } else if (mode == 1) {
  rd:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=32768
      acc ^= mem[b0 + i];
    }
  } else if (mode == 2 || mode == 3) {
    unsigned a = 0;
  lt:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE off
#pragma HLS LOOP_TRIPCOUNT max=100000
      unsigned d = 0;
      if (mode == 2) { const beat_t v = mem[b0 + (a & 1023)]; d = v.range(31, 0) & 0x80000000u; acc ^= v; }
      a = a + 1 + (d >> 31);                       // data-dependent: the next read waits for this one
    }
  }
  for (int k = 0; k < 8; k++) res[k] = acc.range(32 * k + 31, 32 * k);
  res[8] = n;
}
