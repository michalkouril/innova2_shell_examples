#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""triangle_count.py <TC_kernel.xclbin> [--data DIR [--golden N] | --vertices V --edges E --seed S] -- Python (pyxrt)
host for Vitis Libraries graph/L2/benchmarks/triangle_count (ported from host/main.cpp): TC_kernel(n, nnz, offsets,
columns, offsets, columns, scratch[2 x offsets], columns, TC) counts the triangles of an undirected graph stored once,
column > row, columns ascending within a row (the orientation the kernel requires; max degree 65536).

Default: a seeded random graph, checked against an exact count on the host (per edge u-v, the common forward
neighbours of u and v). --data DIR: AMD's csr_offsets.txt / csr_columns.txt (bundled graph: 11 triangles) or
graphdata's europe_osm_csr_*.txt with europe_osm_TC_golden.txt (61,710); --golden N overrides.
"""
import argparse
import glob
import os
import sys

import numpy as np

import graph_common as g
from pyxrt_util import Card, result

ML = 65536


def count_triangles(n, off, col):
    adj = [set(col[off[i]:off[i + 1]].tolist()) for i in range(n)]
    t = 0
    for u in range(n):
        au = adj[u]
        for v in au:
            t += len(au & adj[v])
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--data')
    ap.add_argument('--golden', type=int)
    ap.add_argument('--vertices', type=int, default=20000)
    ap.add_argument('--edges', type=int, default=200000)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    if a.data:
        n, off, col = g.read_csr_txt(g.find(a.data, '*csr_offsets.txt'), g.find(a.data, '*csr_columns.txt'))
        golden = a.golden
        if golden is None:
            f = glob.glob(os.path.join(a.data, '*TC_golden.txt'))
            golden = int(open(f[0]).read().split()[0]) if f else (11 if n == 10 else None)
        if golden is None:
            raise SystemExit('*** no golden count for %s: pass --golden N' % a.data)
        what = 'graph %s' % a.data
    else:
        n = a.vertices
        u, v = g.random_simple_edges(n, a.edges, np.random.default_rng(a.seed))
        off, col = g.csr_from_edges(n, u, v)
        golden = count_triangles(n, off, col)
        what = 'random graph (seed %d)' % a.seed
    nnz = len(col)
    maxdeg = int(np.max(np.diff(off.astype(np.int64))))
    print('INFO: %s: %d vertices, %d edges, max degree %d, expected %d triangles' % (what, n, nnz, maxdeg, golden),
          flush=True)
    if maxdeg > ML:
        raise SystemExit('*** max degree %d > %d: the kernel does not support it' % (maxdeg, ML))
    c = Card(a.xclbin)
    k = c.kernel('TC_kernel')
    V, E = n + 1 + 4096, nnz + 4096
    o1 = c.bo(k, 2, V * 4).zero().put(off)
    r1 = c.bo(k, 3, E * 4).zero().put(col)
    o2 = c.bo(k, 4, V * 4).zero().put(off)
    r2 = c.bo(k, 5, E * 4).zero().put(col)
    o3 = c.bo(k, 6, 2 * V * 4).zero()
    r3 = c.bo(k, 7, E * 4).zero().put(col)
    tc = c.bo(k, 8, 64).zero()
    t = c.timed(k, n, nnz, o1, r1, o2, r2, o3, r3, tc)
    got = int(tc.get(np.uint64, 1)[0])
    print('INFO: kernel time %.3f s; triangles counted %d (expected %d)' % (t, got, golden))
    return result('triangle_count', got == golden, '%d triangles, %.3f s' % (got, t))


if __name__ == '__main__':
    sys.exit(main())
