#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# compound_sort.py <xclbin> [--keys N] -- pyxrt host for AMD Vitis Libraries database/L1/benchmarks/compound_sort
# (host/main.cpp, ported): N random 31-bit keys (AMD: rand(); here a seeded generator), SortKernel(order = 1
# ascending, N, in, out), the output compared with numpy's sort. N defaults to the kernel's maximum LEN = 131072
# (INSERT_LEN 1024 x 4 x 32). Throughput as our README: N x 4 bytes / kernel time, in MiB/s (README: 439, from the
# OpenCL profiling time); the best of --runs (default 5) host-measured runs, which include ~0.2 ms of XRT dispatch.
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

LEN = 131072
p = argparse.ArgumentParser(); p.add_argument('xclbin'); p.add_argument('--keys', type=int, default=LEN)
p.add_argument('--runs', type=int, default=5)
a = p.parse_args()
n = a.keys
assert 0 < n <= LEN, 'the kernel sorts at most %d keys' % LEN
keys = np.random.default_rng(20261017).integers(0, 2**31, n, dtype=np.uint32)
c = Card(a.xclbin); k = c.kernel('SortKernel')
ib = c.bo(k, 2, 4 * LEN).zero().put(keys); ob = c.bo(k, 3, 4 * LEN).zero()
want = np.sort(keys); t, bad = 1e9, 0
for _ in range(max(1, a.runs)):
    ob.zero(); t = min(t, timed_timeout(c, k, 1, n, ib, ob))
    bad += int(np.count_nonzero(ob.get(np.uint32, n) != want))
print('compound_sort: %d keys, kernel %.1f us, %.1f MiB/s; %d out of place' % (n, t * 1e6, n * 4 / t / 2**20, bad))
sys.exit(result('compound_sort', bad == 0, '%.0f MiB/s' % (n * 4 / t / 2**20)))
