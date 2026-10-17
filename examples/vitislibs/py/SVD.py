#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""SVD.py <kernel_svd.xclbin> [--runs N] -- Python (pyxrt) host for Vitis Libraries
quantitative_finance/L1/benchmarks/SVD (ported from host/svd.cpp, benchmark_svd_functions):
kernel_svd_0(A, sigma, U, V, 4) on AMD's fixed symmetric 4 x 4 matrix, checked as the C++ host does:
|| A - U diag(sigma) V^T ||_F <= 1e-4. The first run after loading the xclbin is the "cold" time AMD's README quotes
(196 us); --runs N adds N - 1 warm runs (AMD: 22 us).
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result  # noqa: E402

N = 4
A = np.array([[2545, 2137.34902052323241150588728487, 1821.87553334160179474565666169, 16306.0391790706853498704731464],
              [2137.34902052323241150588728487, 1821.87553334160179474565666169, 1573.78716625872380063810851425,
               12618.9394872652155754622071981],
              [1821.87553334160179474565666169, 1573.78716625872380063810851425, 1375.76089061747416053549386561,
               9923.53468331521253276150673628],
              [16306.0391790706853498704731464, 12618.9394872652155754622071981, 9923.53468331521253276150673628,
               147483.987672218354418873786926]])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--runs', type=int, default=1)
    a = ap.parse_args()
    c = Card(a.xclbin)
    k = c.kernel('kernel_svd_0')
    bA = c.bo(k, 0, N * N * 8).put(A)
    bS, bU, bV = c.bo(k, 1, N * 8), c.bo(k, 2, N * N * 8), c.bo(k, 3, N * N * 8)
    times = [c.timed(k, bA, bS, bU, bV, N) for _ in range(a.runs)]
    s = bS.get(np.float64)
    U = bU.get(np.float64).reshape(N, N)
    V = bV.get(np.float64).reshape(N, N)
    errA = np.linalg.norm(A - (U * s) @ V.T)
    warm = ' , warm mean %.1f us' % (1e6 * np.mean(times[1:])) if len(times) > 1 else ''
    print('INFO: 4x4: first (cold) run %.1f us%s; ||A - U S V^T|| = %.3g; sigma %s'
          % (1e6 * times[0], warm, errA, np.array2string(s, precision=6)))
    return result('SVD', errA <= 1e-4, 'errA %.3g, cold %.1f us' % (errA, 1e6 * times[0]))


if __name__ == '__main__':
    sys.exit(main())
