// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// lat_probe: memory round-trip latency as seen by an RP kernel. Every operation is strictly serialized (one beat,
// then wait for its completion), so the host divides the run time by n.
//   mode 0: write one beat to mem[off/64], wait for the write response (AW+W -> B)
//   mode 1: read one beat from mem[off/64]                               (AR -> R)
//   mode 2: spin `n` cycles (an LFSR step per cycle, II=1) -- measures the kernel clock against host time
//   mode 3: n empty iterations of the same loop shell as modes 0/1 -- the per-iteration overhead
// res[0] = an accumulated value (keeps HLS from removing the work).
#include <ap_int.h>
#include <hls_burst_maxi.h>
#include <stdint.h>

typedef ap_uint<512> beat_t;

extern "C" void lat_probe_l1(hls::burst_maxi<beat_t> mem, uint32_t *res, unsigned off, unsigned n, unsigned mode)
{
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem0 depth=65536 max_read_burst_length=2 max_write_burst_length=2 num_read_outstanding=1 num_write_outstanding=1 latency=1
#pragma HLS INTERFACE m_axi port=res offset=slave bundle=gmem1 depth=16
  uint32_t acc = 0, spin = 0xACE1u;
  const unsigned a = off / 64;
  if (mode == 2) {
  sp:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
      spin = (spin >> 1) ^ ((0u - (spin & 1u)) & 0xB4BCD35Cu);
    }
    acc = spin;
  } else {
  op:
    for (unsigned i = 0; i < n; i++) {
#pragma HLS PIPELINE off
      if (mode == 0) {
        beat_t w = i;
        mem.write_request(a, 1);
        mem.write(w);
        mem.write_response();
      } else if (mode == 1) {
        mem.read_request(a, 1);
        const beat_t r = mem.read();
        acc += r.range(31, 0);
      } else {
        acc += i;
      }
    }
  }
  res[0] = acc;
}
