#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/kernel_chain -- Python (pyxrt) port of Vitis_Accel_Examples host_xrt/kernel_chain/src/host.cpp (commit
d1a35c39, an OpenCL host): three 32x32 int matrix products in one kernel, 10 times with random inputs, run once with
krnl_chain_mmult (ap_ctrl_chain: the 10 runs overlap) and once with krnl_simple_mmult (ap_ctrl_hs); both results are
compared with the host's products (int arithmetic, wrapping), and the two wall-clock times printed. As in AMD's host,
each iteration syncs its inputs and starts its run without waiting; all 10 are waited for at the end."""
import sys
import time
import numpy as np
from suite_lib import Card, parse, result, xclbin, rand31, i32

a = parse(__doc__)
c = Card(xclbin(a.path, 'krnl_mmult.xclbin'))
D, T = 32, 10
ins = [[rand31(D * D, seed=4 * t + j).reshape(D, D) for j in range(4)] for t in range(T)]
def mm(x, y):
    return i32(x.astype(np.int64) @ y.astype(np.int64))      # int64 wraps mod 2^64, so the low 32 bits are C's
sw = [mm(mm(mm(m[0], m[1]), m[2]), m[3]) for m in ins]
ok, times = True, {}
for name in ('krnl_chain_mmult', 'krnl_simple_mmult'):
    k = c.kernel(name)
    bos = [[c.bo(k, j, D * D * 4) for j in range(4)] + [c.bo(k, 4, D * D * 4).zero()] for _ in range(T)]
    t0 = time.perf_counter(); runs = []
    for t in range(T):
        for j in range(4):
            bos[t][j].put(ins[t][j])
        runs.append(c.start(k, *bos[t], D))
    for r in runs:
        r.wait()
    times[name] = time.perf_counter() - t0
    for t in range(T):
        if not np.array_equal(bos[t][4].get(np.int32).reshape(D, D), sw[t]):
            print('%s: iteration %d mismatch' % (name, t)); ok = False; break
print('| krnl_chain_mmult  (10 iterations) | %.6f s |\n| krnl_simple_mmult (10 iterations) | %.6f s |\nspeedup %.3f'
      % (times['krnl_chain_mmult'], times['krnl_simple_mmult'], times['krnl_simple_mmult'] / times['krnl_chain_mmult']))
sys.exit(result('host_xrt/kernel_chain', ok, 'chain %.1f us, simple %.1f us' %
                (times['krnl_chain_mmult'] * 1e6, times['krnl_simple_mmult'] * 1e6)))
