#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/mult_compute_units_xrt -- Python (pyxrt) port of Vitis_Accel_Examples
host_xrt/mult_compute_units_xrt/src/host.cpp (commit d1a35c39): 64K ints in 4 chunks, one run per chunk of the vadd
kernel; the 4 runs are started back to back on one pyxrt.kernel, which XRT spreads over the kernel's 4 compute units,
then all waited for; each chunk must equal a + b (a[i] = b[i] = i within the chunk)."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'vadd.xclbin'))
k = c.kernel('vadd')
CU, N = 4, 1024 * 64
n = N // CU
x = np.arange(n, dtype=np.int32)
bufs = [(c.bo(k, 0, n * 4).put(x), c.bo(k, 1, n * 4).put(x), c.bo(k, 2, n * 4).zero()) for _ in range(CU)]
runs = [c.start(k, b0, b1, bo, n) for b0, b1, bo in bufs]
for r in runs:
    r.wait()
ok = all(np.array_equal(bo.get(np.int32), x + x) for _, _, bo in bufs)
sys.exit(result('host_xrt/mult_compute_units_xrt', ok, '%d runs in flight on one kernel object' % CU))
