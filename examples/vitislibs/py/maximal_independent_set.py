#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""maximal_independent_set.py <mis_kernel.xclbin> [--data DIR | --vertices V --edges E --seed S] -- Python (pyxrt) host
for Vitis Libraries graph/L2/benchmarks/maximal_independent_set (ported from host/main.cpp): mis_kernel(n, offsets,
indices, C0, C1, S0, S1, res_out) writes the selected vertices to res_out, terminated by -256.

The kernel keeps its write position (count_S) in a register across runs until the xclbin is reloaded, so a second run
appends after the first one's result: res_out is pre-filled with a marker and the result is the span the kernel
wrote; it is sized as AMD's host sizes it (M_VERTEX) so repeated runs cannot write past it.
Check: the set is independent (no two selected vertices adjacent) and maximal (every other vertex has a selected
neighbour); with --data DIR (AMD's data-csr-offset.mtx, data-csr-indicesweights.mtx, mis.txt) it is also compared with
the golden list, as the C++ host does. Default input: a seeded random undirected graph stored in both directions,
of the bundled graph's size (1015 vertices, about 11,000 entries; the kernel's internal FIFOs hold 40,000).
"""
import argparse
import os
import sys

import numpy as np

import graph_common as g
from pyxrt_util import TO_DEV, Card, result

M_VERTEX = 57108864
V_NULL = -256
MARK = -1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--data')
    ap.add_argument('--vertices', type=int, default=1015)
    ap.add_argument('--edges', type=int, default=5500)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    golden = None
    if a.data:
        n, off = g.read_offset_mtx(g.find(a.data, '*csr-offset.mtx'))
        col, _ = g.read_indicesweights_mtx(g.find(a.data, '*csr-indicesweights.mtx'))
        mf = os.path.join(a.data, 'mis.txt')
        if os.path.exists(mf):
            golden = np.array(open(mf).read().split()[1:], dtype=np.int64)
        what = 'graph %s' % a.data
    else:
        n = a.vertices
        off, col = g.symmetric_csr(n, a.edges, a.seed)
        what = 'random graph (seed %d)' % a.seed
    nnz = len(col)
    print('INFO: %s: %d vertices, %d entries' % (what, n, nnz), flush=True)
    c = Card(a.xclbin)
    k = c.kernel('mis_kernel')
    V = n + 1 + 4096
    bo_off = c.bo(k, 1, V * 4).zero().put(off)
    bo_idx = c.bo(k, 2, (nnz + 4096) * 4).zero().put(col)
    groups = [c.bo(k, 3 + i, V * 4).zero() for i in range(4)]
    res = c.bo(k, 7, M_VERTEX * 4)
    res.mem.view(np.int32)[:] = MARK
    res.bo.sync(TO_DEV, res.size, 0)
    t = c.timed(k, n, bo_off, bo_idx, *groups, res)
    out = res.get(np.int32)
    w = np.nonzero(out != MARK)[0]
    if len(w) == 0 or out[w[-1]] != V_NULL:
        return result('maximal_independent_set', False, 'no terminated result in res_out')
    sel = out[w[0]:w[-1]].astype(np.int64)
    print('INFO: kernel time %.3f ms; %d vertices selected (result at res_out[%d])' % (1e3 * t, len(sel), w[0]))
    insel = np.zeros(n, dtype=bool)
    insel[sel] = True
    src = np.repeat(np.arange(n), np.diff(off.astype(np.int64)))
    dst = col.astype(np.int64)
    nl = src != dst
    adjacent = int(np.sum(insel[src[nl]] & insel[dst[nl]]))
    covered = np.zeros(n, dtype=bool)
    covered[dst[insel[src]]] = True
    covered[src[insel[dst]]] = True
    uncovered = int(np.sum(~insel & ~covered))
    ok = adjacent == 0 and uncovered == 0 and len(np.unique(sel)) == len(sel)
    detail = '%d selected, %.3f ms, adjacent pairs %d, unselected without a selected neighbour %d' \
        % (len(sel), 1e3 * t, adjacent, uncovered)
    if golden is not None:
        same = len(golden) == len(sel) and np.array_equal(golden, sel)
        print('INFO: against mis.txt (%d vertices): %s' % (len(golden), 'identical' if same else 'DIFFERENT'))
        ok = ok and same
        detail += ', golden %s' % ('identical' if same else 'differs')
    return result('maximal_independent_set', ok, detail)


if __name__ == '__main__':
    sys.exit(main())
