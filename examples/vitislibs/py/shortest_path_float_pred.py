#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""shortest_path_float_pred.py <shortestPath_top.xclbin> [--data DIR | --vertices V --edges E --seed S] -- Python
(pyxrt) host for Vitis Libraries graph/L2/benchmarks/shortest_path_float_pred (ported from host/main.cpp):
shortestPath_top(config, offset, column, weight, queue, queue, result, result, pred, pred, info) is single-source
shortest path with float weights and predecessors, from the vertex of highest out-degree (as AMD's host chooses it).

Checks, as the C++ host: every distance within 1e-5 (relative) of the reference, and every predecessor consistent
(dist[pred[v]] + w(pred[v] -> v) = dist[v] within 1e-5; a different predecessor than the golden's is accepted when
it gives the same distance, as the C++ host accepts an equally short path). Queue / table overflow (info) fails.
Reference: by default a seeded random simple directed graph (no duplicate edges, no self-loops) with weights in
(0, 1], solved with Dijkstra on the host. Kernel limitation found with this host: with --multigraph (duplicate edges
and self-loops) and non-unit weights, some distances come out longer than the shortest path (165 of 20,000 vertices,
up to +82 %, each consistent with its own predecessor path); simple graphs, and unit weights, are exact.
--data DIR: AMD's data-csr-offset.mtx / data-csr-indicesweights.mtx / data-golden.sssp.mtx, or graphdata's lj-*
(soc-LiveJournal1, weights 1.0; about 2.5 GB of host memory, a minute to load).
"""
import argparse
import heapq
import sys

import numpy as np

import graph_common as g
from pyxrt_util import Card, result

QUEUE = 10 * 300 * 4096
INF = np.float32(np.inf)


def dijkstra(n, off, col, w, s):
    dist = np.full(n, np.inf)
    dist[s] = 0.0
    h = [(0.0, s)]
    o, c, ww = off.tolist(), col.tolist(), w.astype(np.float64).tolist()
    done = np.zeros(n, dtype=bool)
    while h:
        d, u = heapq.heappop(h)
        if done[u]:
            continue
        done[u] = True
        for i in range(o[u], o[u + 1]):
            v = c[i]
            nd = d + ww[i]
            if nd < dist[v]:
                dist[v] = nd
                heapq.heappush(h, (nd, v))
    return dist.astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--data')
    ap.add_argument('--vertices', type=int, default=20000)
    ap.add_argument('--edges', type=int, default=200000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--weights', choices=('random', 'one'), default='random',
                    help='synthetic graph: weights in (0, 1] or all 1.0')
    ap.add_argument('--multigraph', action='store_true',
                    help='synthetic graph with duplicate edges and self-loops (the kernel gets these wrong, see above)')
    a = ap.parse_args()
    if a.data:
        n, off = g.read_offset_mtx(g.find(a.data, '*csr-offset.mtx'))
        col, w = g.read_indicesweights_mtx(g.find(a.data, '*csr-indicesweights.mtx'))
        gold = g.numbers(g.find(a.data, '*golden.sssp.mtx'), np.float64, skip_lines=1).reshape(-1, 3)
        ref = np.full(n, np.inf, dtype=np.float32)
        ref[gold[:, 0].astype(np.int64) - 1] = gold[:, 1]
        chk = np.zeros(n, dtype=bool)
        chk[gold[:, 0].astype(np.int64) - 1] = True      # the C++ host checks only the vertices the golden lists
        what = 'graph %s' % a.data
    else:
        n = a.vertices
        r = np.random.default_rng(a.seed)
        src, dst = r.integers(0, n, a.edges), r.integers(0, n, a.edges)
        if not a.multigraph:
            k_ = np.unique(src * n + dst)
            src, dst = k_ // n, k_ % n
            keep = src != dst
            src, dst = src[keep], dst[keep]
        off, col = g.csr_from_edges(n, src, dst)
        w = (1.0 - r.random(len(col))).astype(np.float32) if a.weights == 'random' else np.ones(len(col), np.float32)
        ref = None
        chk = np.ones(n, dtype=bool)
        what = 'random graph (seed %d)' % a.seed
    nnz = len(col)
    source = int(np.argmax(np.diff(off.astype(np.int64))))
    if ref is None:
        ref = dijkstra(n, off, col, w, source)
    print('INFO: %s: %d vertices, %d edges, source %d (max out-degree)' % (what, n, nnz, source), flush=True)
    cfg = np.zeros(16, dtype=np.uint32)
    cfg[:6] = [n, np.array([np.inf], np.float32).view(np.uint32)[0], 0, QUEUE, 0b011, source]
    c = Card(a.xclbin)
    k = c.kernel('shortestPath_top')
    R = (n + 1023) // 1024 * 1024
    b_cfg = c.bo(k, 0, cfg.nbytes).put(cfg)
    b_off = c.bo(k, 1, (n + 1 + 16) * 4).zero().put(off)
    b_col = c.bo(k, 2, (nnz + 16) * 4).zero().put(col)
    b_w = c.bo(k, 3, (nnz + 16) * 4).zero().put(w)
    b_q = c.bo(k, 4, QUEUE * 4)
    b_res, b_pred = c.bo(k, 6, R * 4), c.bo(k, 8, R * 4)
    b_info = c.bo(k, 10, 64).zero()
    t = c.timed(k, b_cfg, b_off, b_col, b_w, b_q, b_q, b_res, b_res, b_pred, b_pred, b_info)
    info = b_info.get(np.uint8, 4)
    res = b_res.get(np.float32, n)
    pred = b_pred.get(np.uint32, n).astype(np.int64)
    print('INFO: kernel time %.3f s' % t, flush=True)
    if info[0] or info[1]:
        return result('shortest_path_float_pred', False, 'queue overflow' if info[0] else 'table overflow')
    fin = np.isfinite(ref)
    both_inf = ~fin & ~np.isfinite(res)
    rel = np.where(fin & (ref > 0), np.abs(res - ref) / np.where(ref > 0, ref, 1), np.where(fin, np.abs(res - ref), 0))
    dist_bad = int(np.sum(chk & ((rel > 1e-5) | (~fin & ~both_inf))))
    # predecessor consistency: the minimum weight of the edge pred[v] -> v, plus dist[pred[v]], must give dist[v]
    s_all = np.repeat(np.arange(n, dtype=np.int64), np.diff(off.astype(np.int64)))
    key = s_all * n + col.astype(np.int64)
    o = np.argsort(key, kind='stable')
    key, wk = key[o], w[o].astype(np.float64)
    first = np.r_[True, key[1:] != key[:-1]]
    ukey, wmin = key[first], np.minimum.reduceat(wk, np.nonzero(first)[0])
    v = np.nonzero(chk & fin & (np.arange(n) != source))[0]
    q = pred[v] * n + v
    pos = np.clip(np.searchsorted(ukey, q), 0, len(ukey) - 1)
    has = (ukey[pos] == q) & (pred[v] < n)
    pd = np.where(has, res[np.minimum(pred[v], n - 1)].astype(np.float64) + wmin[pos], np.inf)
    pred_bad = int(np.sum(~has | (np.abs(pd - res[v]) / np.maximum(res[v], 1e-30) > 1e-5)))
    reach = int(np.sum(fin & chk))
    print('INFO: %d reachable; distance errors %d, predecessor errors %d' % (reach, dist_bad, pred_bad))
    bad_v = np.nonzero(chk & ((rel > 1e-5) | (~fin & ~both_inf)))[0]
    if len(bad_v):
        print('    worst distance errors (vertex, reference, kernel, relative):')
        for i in bad_v[np.argsort(-rel[bad_v])][:6]:
            print('      %d %.7g %.7g %.3g' % (i, ref[i], res[i], rel[i]))
    return result('shortest_path_float_pred', dist_bad == 0 and pred_bad == 0,
                  '%d reachable, %.3f s, %d distance / %d predecessor errors' % (reach, t, dist_bad, pred_bad))


if __name__ == '__main__':
    sys.exit(main())
