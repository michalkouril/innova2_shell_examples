#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""gtsv.py <kernel_gtsv.xclbin> [--runs N] [--random SEED] -- Python (pyxrt) host for Vitis Libraries
solver/L2/benchmarks/gtsv (ported from its test_gtsv.cpp): kernel_gtsv_0(n, low, diag, up, rhs) solves the tridiagonal
system in place (the solution replaces rhs).

Default: AMD's test, the n = 16 system with -1 / 2 / -1 diagonals and rhs (1, 0, ..., 0, 1), whose solution is all
ones (|x - 1| <= 1e-7, as the C++ host checks). --random SEED: a random diagonally dominant system, checked against
numpy.linalg.solve (relative 1e-9). The xclbin is compiled for n = 16 only (NRC 16): any other n hangs the kernel, so
there is no size option. --runs N times N runs (the inputs are re-written between runs, untimed) and prints the mean.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result  # noqa: E402

N = 16


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--random', type=int, metavar='SEED')
    a = ap.parse_args()
    if a.random is None:
        low, diag, up, rhs = np.full(N, -1.0), np.full(N, 2.0), np.full(N, -1.0), np.zeros(N)
        low[0] = up[N - 1] = 0.0
        rhs[0] = rhs[N - 1] = 1.0
        want = np.ones(N)
    else:
        r = np.random.default_rng(a.random)
        low, up = r.uniform(-1, 1, N), r.uniform(-1, 1, N)
        low[0] = up[N - 1] = 0.0
        diag = np.abs(low) + np.abs(up) + r.uniform(0.5, 2, N)
        rhs = r.uniform(-10, 10, N)
        m = np.diag(diag) + np.diag(low[1:], -1) + np.diag(up[:-1], 1)
        want = np.linalg.solve(m, rhs)
    c = Card(a.xclbin)
    k = c.kernel('kernel_gtsv_0')
    bl, bd, bu, br = (c.bo(k, i + 1, N * 8) for i in range(4))
    times = []
    for _ in range(a.runs):
        bl.put(low); bd.put(diag); bu.put(up); br.put(rhs)
        times.append(c.timed(k, N, bl, bd, bu, br))
    x = br.get(np.float64)
    err = np.max(np.abs(x - want)) if a.random is None else np.max(np.abs(x - want) / np.maximum(np.abs(want), 1e-300))
    ok = err <= (1e-7 if a.random is None else 1e-9)
    print('INFO: n %d, %d run(s): mean kernel time %.1f us (first %.1f us); max %s error %.3g'
          % (N, a.runs, 1e6 * np.mean(times), 1e6 * times[0], 'abs' if a.random is None else 'relative', err))
    return result('gtsv', ok, 'n=%d max err %.3g' % (N, err))


if __name__ == '__main__':
    sys.exit(main())
