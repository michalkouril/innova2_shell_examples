#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""lat_host.py <user BDF> <xclbin> [reps]  |  lat_host.py <user BDF> <xclbin> once <mode> <off> <n>

The Python (pyxrt) twin of fld_lite/lat/lat_host.cpp, same measurements and output: per-operation memory latency of
the lat_probe kernel, in ns and kernel cycles. One 4 MB bank0 buffer allocated first so it starts at bank0 offset 0
(offset 0 = the UltraRAM window in HYBRID DDR mode, offset 2 MB = DDR). Launch overhead cancels: time(n2) - time(n1)
over (n2 - n1) operations, median of `reps` (default 7). LAT_K selects another kernel name (e.g. lat_probe_l1).
The kernel does the timing work, so Python's slower dispatch only adds to the launch overhead that cancels out.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card  # noqa: E402

if len(sys.argv) < 3:
    sys.exit('usage: %s <user BDF> <xclbin> [reps]' % sys.argv[0])
once = len(sys.argv) > 6 and sys.argv[3] == 'once'
reps = int(sys.argv[3]) if len(sys.argv) > 3 and not once else 7
c = Card(sys.argv[2], dev=sys.argv[1])
kn = os.environ.get('LAT_K', 'lat_probe')
k = c.kernel(kn)
print('kernel %s' % kn)
bo = c.bo(k, 0, 4 << 20)
res = c.bo(k, 1, 4096)
if once:                                              # for ILA captures: one run of <mode> at <off>, <n> operations
    c.call(k, bo, res, int(sys.argv[5], 0), int(sys.argv[6]), int(sys.argv[4]))
    print('once: mode %s off %s n %s done' % (sys.argv[4], sys.argv[5], sys.argv[6]))
    sys.exit(0)
print('buffer at 0x%x (%s)' % (bo.address(), 'bank0 offset 0: offset 0 is the URAM window in HYBRID mode'
                                if bo.address() == 0x200000000 else 'NOT at bank0 offset 0'))


def run_us(off, n, mode):
    t0 = time.perf_counter()
    c.call(k, bo, res, off, n, mode)
    return (time.perf_counter() - t0) * 1e6


def per_op_ns(off, n1, n2, mode):
    v = sorted((run_us(off, n2, mode) - run_us(off, n1, mode)) * 1000.0 / (n2 - n1) for _ in range(reps))
    return v[len(v) // 2]


ns_per_cycle = per_op_ns(0, 1000000, 101000000, 2)       # kernel clock: spin loop, II=1
empty = per_op_ns(0, 10000, 1010000, 3)
print('kernel clock: %.1f MHz (%.3f ns/cycle); empty loop iteration %.1f ns = %.1f cycles' %
      (1000.0 / ns_per_cycle, ns_per_cycle, empty, empty / ns_per_cycle))
for name, off, mode in (('write+ack  URAM window (off 0)', 0, 0), ('read       URAM window (off 0)', 0, 1),
                        ('write+ack  DDR (off 2 MB)', 2 << 20, 0), ('read       DDR (off 2 MB)', 2 << 20, 1)):
    ns = per_op_ns(off, 10000, 210000, mode)
    print('%-32s %7.1f ns/op = %6.1f cycles  (minus loop overhead: %6.1f cycles)' %
          (name, ns, ns / ns_per_cycle, (ns - empty) / ns_per_cycle))
