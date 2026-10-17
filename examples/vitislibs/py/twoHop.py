#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""twoHop.py <twoHop_kernel.xclbin> [--data DIR | --vertices V --edges E --pairs P --seed S] -- Python (pyxrt) host for
Vitis Libraries graph/L2/benchmarks/twoHop (ported from host/main.cpp): twoHop_kernel(numPairs, pairs, offsets,
indices, offsets, indices, counts) returns, for every pair (s, d), the number of directed 2-hop walks s -> m -> d.

Default: a seeded random directed graph and P pairs (half uniform, half along an existing 2-hop walk, as
graphdata/tools/twohop_ref.c makes them), checked against an exact count on the host. --data DIR: AMD's
data-csr-offset.mtx / data-csr-indicesweights.mtx / data-pair.mtx / data-golden.twoHop.mtx, or graphdata's lj-*
(soc-LiveJournal1, 1000 pairs); every pair's count must equal the golden's, as the C++ host checks.
"""
import argparse
import sys

import numpy as np

import graph_common as g
from pyxrt_util import Card, result


def two_hop(off, col, s, d):
    o = off.astype(np.int64)
    t = 0
    for m in col[o[s]:o[s + 1]].tolist():
        row = col[o[m]:o[m + 1]]
        t += int(np.searchsorted(row, d, 'right') - np.searchsorted(row, d, 'left'))
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--data')
    ap.add_argument('--vertices', type=int, default=20000)
    ap.add_argument('--edges', type=int, default=200000)
    ap.add_argument('--pairs', type=int, default=1000)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    if a.data:
        n, off = g.read_offset_mtx(g.find(a.data, '*csr-offset.mtx'))
        col, _ = g.read_indicesweights_mtx(g.find(a.data, '*csr-indicesweights.mtx'))
        pf = g.numbers(g.find(a.data, '*pair.mtx'), np.int64)
        pairs = pf[1:1 + 2 * int(pf[0])].reshape(-1, 2) - 1
        gl = g.numbers(g.find(a.data, '*golden.twoHop.mtx'), np.int64).reshape(-1, 3)
        golden = {(int(x) - 1, int(y) - 1): int(z) for x, y, z in gl}
        what = 'graph %s' % a.data
    else:
        n = a.vertices
        r = np.random.default_rng(a.seed)
        off, col = g.csr_from_edges(n, r.integers(0, n, a.edges), r.integers(0, n, a.edges))
        o = off.astype(np.int64)
        has_out = np.nonzero(np.diff(o) > 0)[0]
        pairs = []
        for i in range(a.pairs):
            if i % 2 == 0:
                pairs.append((int(r.integers(n)), int(r.integers(n))))
            else:
                s = int(has_out[r.integers(len(has_out))])
                m = int(col[o[s] + r.integers(o[s + 1] - o[s])])
                d = int(col[o[m] + r.integers(o[m + 1] - o[m])]) if o[m + 1] > o[m] else int(r.integers(n))
                pairs.append((s, d))
        pairs = np.array(pairs, dtype=np.int64)
        golden = {(int(s), int(d)): two_hop(off, col, int(s), int(d)) for s, d in pairs}
        what = 'random graph (seed %d)' % a.seed
    np_ = len(pairs)
    print('INFO: %s: %d vertices, %d edges, %d pairs' % (what, n, len(col), np_), flush=True)
    c = Card(a.xclbin)
    k = c.kernel('twoHop_kernel')
    p64 = (pairs[:, 0].astype(np.uint64) << np.uint64(32)) | pairs[:, 1].astype(np.uint64)
    b_pair = c.bo(k, 1, (np_ + 8) * 8).zero().put(p64)
    b_o1 = c.bo(k, 2, (n + 1 + 16) * 4).zero().put(off)
    b_i1 = c.bo(k, 3, (len(col) + 16) * 4).zero().put(col)
    b_o2 = c.bo(k, 4, (n + 1 + 16) * 4).zero().put(off)
    b_i2 = c.bo(k, 5, (len(col) + 16) * 4).zero().put(col)
    b_cnt = c.bo(k, 6, (np_ + 16) * 4).zero()
    t = c.timed(k, np_, b_pair, b_o1, b_i1, b_o2, b_i2, b_cnt)
    cnt = b_cnt.get(np.uint32, np_)
    bad = missing = 0
    for (s, d), x in zip(pairs.tolist(), cnt.tolist()):
        want = golden.get((s, d))
        if want is None:
            missing += 1
        elif want != x:
            bad += 1
            if bad <= 5:
                print('    pair %d -> %d: got %d, want %d' % (s + 1, d + 1, x, want))
    nz = int(np.sum(cnt > 0))
    print('INFO: kernel time %.3f ms; %d pairs with a 2-hop walk; %d wrong, %d not in the golden'
          % (1e3 * t, nz, bad, missing))
    return result('twoHop', bad == 0 and missing == 0, '%d pairs, %.3f ms, %d wrong, %d missing' % (np_, 1e3 * t, bad,
                                                                                                     missing))


if __name__ == '__main__':
    sys.exit(main())
