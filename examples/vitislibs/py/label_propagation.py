#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""label_propagation.py <LPKernel.xclbin> [--iter N] [--data DIR | --vertices V --edges E --seed S] [--label FILE]
-- Python (pyxrt) host for Vitis Libraries graph/L2/benchmarks/label_propagation (ported from host/main.cpp):
LPKernel(n, nnz, iterations, offsetCSR, indexCSR, offsetCSC, indexCSC, indexCSC, pingHash, pongHash, labelPing,
labelPong) builds the CSC itself and runs N iterations; the labels end in labelPong after an odd count, labelPing after
an even one.

Check of one iteration (no golden needed): every vertex's label is one of the most frequent labels among its
in- and out-neighbours (a self-loop counted once; the labels start as the vertex ids), or its own label if it has
none. The tie-break is the kernel's own random choice, so this is the strongest check that does not reproduce the
kernel; it holds on 1,139,905 of
1,139,905 vertices of hollywood-2009 against lp_csim's exact labels (examples/vitislibs/graphdata).
Beyond the first iteration the kernel's update does not follow that rule from the previous labels (neither the other
ping/pong buffer nor a replay with N - 1 iterations satisfies it), so for N > 1 the labels are compared with a golden
only: --label FILE (or a matching file in --data) compares with exact golden labels, as the C++ host does, at any N.

Inputs: default a seeded random graph (V vertices, E directed edges stored once with column >= row, some self-loops);
--data DIR takes AMD's files (csr_offsets.txt, csr_columns.txt, label.txt) or graphdata's (hollywood_csr_*.txt and
hollywood_label_iter<N>.txt).
"""
import argparse
import glob
import os
import sys

import numpy as np

import graph_common as g
from pyxrt_util import Card, result


def most_frequent_ok(n, off, col, lab, prev):
    """Vertices whose label is not among the most frequent previous labels of their neighbours (self-loop once)."""
    src = np.repeat(np.arange(n, dtype=np.int64), np.diff(off.astype(np.int64)))
    col = col.astype(np.int64)
    ns = src != col
    a = np.concatenate([src, col[ns]])
    b = np.concatenate([col, src[ns]])
    prev = prev.astype(np.int64)
    M = int(max(prev.max(), lab.max())) + 1
    k, cnt = np.unique(a * M + prev[b], return_counts=True)
    va = k // M
    mx = np.zeros(n, dtype=np.int64)
    np.maximum.at(mx, va, cnt)
    best = cnt == mx[va]
    q = np.arange(n, dtype=np.int64) * M + lab.astype(np.int64)
    pos = np.searchsorted(k, q)
    pos[pos >= len(k)] = 0
    hit = (k[pos] == q) & best[pos]
    isolated = (np.bincount(a, minlength=n) == 0) & (lab.astype(np.int64) == prev)
    return np.nonzero(~(hit | isolated))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--iter', type=int, default=1)
    ap.add_argument('--data')
    ap.add_argument('--label', help='exact golden labels ("n n" header, then "vertex label")')
    ap.add_argument('--vertices', type=int, default=20000)
    ap.add_argument('--edges', type=int, default=200000)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    golden = a.label
    if a.data:
        n, off, col = g.read_csr_txt(g.find(a.data, '*csr_offsets.txt'), g.find(a.data, '*csr_columns.txt'))
        if golden is None:
            m = glob.glob(os.path.join(a.data, '*label_iter%d.txt' % a.iter)) or \
                (glob.glob(os.path.join(a.data, 'label.txt')) if a.iter == 1 else [])
            golden = m[0] if m else None
        what = 'graph %s' % a.data
    else:
        r = np.random.default_rng(a.seed)
        n = a.vertices
        u, v = g.random_simple_edges(n, a.edges, r)
        loops = r.choice(n, n // 10, replace=False)
        off, col = g.csr_from_edges(n, np.concatenate([u, loops]), np.concatenate([v, loops]))
        what = 'random graph (seed %d)' % a.seed
    nnz = len(col)
    print('INFO: %s: %d vertices, %d edges, %d iteration(s)' % (what, n, nnz, a.iter), flush=True)
    c = Card(a.xclbin)
    k = c.kernel('LPKernel')
    V, E = len(g.pad(np.zeros(n + 1 + 4096, np.uint32))), len(g.pad(np.zeros(nnz + 4096, np.uint32)))
    bo_off = c.bo(k, 3, V * 4).zero().put(off)
    bo_col = c.bo(k, 4, E * 4).put(col)
    bo_offc, bo_colc = c.bo(k, 5, V * 4).zero(), c.bo(k, 6, E * 4).zero()
    bo_ping_h, bo_pong_h = c.bo(k, 8, V * 4).zero(), c.bo(k, 9, V * 4).zero()
    bo_ping, bo_pong = c.bo(k, 10, V * 4).zero(), c.bo(k, 11, V * 4).zero()
    t = c.timed(k, n, nnz, a.iter, bo_off, bo_col, bo_offc, bo_colc, bo_colc, bo_ping_h, bo_pong_h, bo_ping, bo_pong)
    lab = (bo_pong if a.iter % 2 else bo_ping).get(np.uint32, n)
    print('INFO: kernel time %.3f s; %d distinct labels' % (t, len(np.unique(lab))), flush=True)
    if a.iter == 1:
        bad = most_frequent_ok(n, off, col, lab, np.arange(n, dtype=np.uint32))
        for v in bad[:5]:
            print('    vertex %d: label %d is not a most frequent neighbour label' % (v, lab[v]))
        check = 'most-frequent check %d bad' % len(bad)
    else:   # beyond iteration 1 the kernel's update is not reproduced on the host: labels must be vertex ids
        bad = np.nonzero(lab >= n)[0]
        check = '%d labels out of range (iterations > 1: compared with a golden only)' % len(bad)
        if not golden:
            print('INFO: no golden labels for %d iterations: only the label range is checked' % a.iter)
    ok = len(bad) == 0
    detail = 'n=%d, %d iter, %.3f s, %s' % (n, a.iter, t, check)
    if golden:
        gl = g.numbers(golden, np.int64)[3::2][:n]
        diff = int(np.sum(gl != lab))
        print('INFO: against %s: %d of %d labels differ' % (golden, diff, n))
        ok = ok and diff == 0
        detail += ', golden %d differ' % diff
    return result('label_propagation', ok, detail)


if __name__ == '__main__':
    sys.exit(main())
