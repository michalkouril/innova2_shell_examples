#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""gesvdj.py <kernel_gesvdj.xclbin> [--seed S] [--runs N] -- Python (pyxrt) host for Vitis Libraries
solver/L2/benchmarks/gesvdj (ported from its test_gesvdj.cpp): kernel_gesvdj_0(A, sigma, U, V, n) is the one-sided
Jacobi SVD of a symmetric n x n matrix (row-major doubles; sigma comes back as the full n x n diagonal matrix).

Checks, as the C++ host: || A - U sigma V^T ||_F <= 1e-4; also the singular values against numpy.linalg.svd (1e-9
relative). The input is a random symmetric matrix (seed S, default 12 as AMD's host; the values differ from AMD's
symMatGen). The xclbin is compiled for n = 16 only (MA 16): any other size hangs the kernel, so there is no size option.
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
    ap.add_argument('--seed', type=int, default=12)
    ap.add_argument('--runs', type=int, default=1)
    a = ap.parse_args()
    r = np.random.default_rng(a.seed)
    m = r.uniform(-2048, 2048, (N, N))
    A = (m + m.T) / 2
    c = Card(a.xclbin)
    k = c.kernel('kernel_gesvdj_0')
    bA = c.bo(k, 0, N * N * 8).put(A)
    bS, bU, bV = c.bo(k, 1, N * N * 8), c.bo(k, 2, N * N * 8), c.bo(k, 3, N * N * 8)
    times = [c.timed(k, bA, bS, bU, bV, N) for _ in range(a.runs)]
    S = bS.get(np.float64).reshape(N, N)
    U = bU.get(np.float64).reshape(N, N)
    V = bV.get(np.float64).reshape(N, N)
    errA = np.linalg.norm(A - U @ S @ V.T)
    sv = np.sort(np.abs(np.diag(S)))[::-1]
    ref = np.linalg.svd(A, compute_uv=False)
    errS = np.max(np.abs(sv - ref) / ref[0])
    print('INFO: %dx%d symmetric, %d run(s): mean kernel time %.1f us (first %.1f us); ||A - U S V^T|| = %.3g, '
          'singular values vs numpy %.3g' % (N, N, a.runs, 1e6 * np.mean(times), 1e6 * times[0], errA, errS))
    return result('gesvdj', errA <= 1e-4 and errS <= 1e-9, 'errA %.3g' % errA)


if __name__ == '__main__':
    sys.exit(main())
