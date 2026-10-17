# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""graph_common -- input files and synthetic graphs for the Python hosts of the Vitis Libraries graph benchmarks
(label_propagation.py, triangle_count.py, maximal_independent_set.py, shortest_path_float_pred.py, twoHop.py).

File formats are those of AMD's hosts (graph/L2/benchmarks/*/host/main.cpp) and of examples/vitislibs/graphdata:
  *-csr-offset.mtx        "n n" header, then n+1 offsets
  *-csr-indicesweights.mtx "nnz" header, then "column weight" per line
  csr_offsets.txt         "n" header, then n+1 offsets           (triangle_count, label_propagation)
  csr_columns.txt         "nnz" header, then one column per line
Parsing uses numpy's C text reader (np.fromfile with sep), so the 0.6 GB SuiteSparse files load in seconds.
"""
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'common'))


def numbers(path, dtype=np.float64, skip_lines=0):
    """Every whitespace- (or comma-) separated number of a text file after its first skip_lines lines, as one array."""
    with open(path, 'rb') as f:
        for _ in range(skip_lines):
            f.readline()
        raw = f.read()
    if b',' in raw:
        raw = raw.replace(b',', b' ')
    return np.fromstring(raw, dtype=dtype, sep=' ')


def read_offset_mtx(path):
    v = numbers(path, np.int64)
    n = int(v[1])
    return n, v[2:2 + n + 1].astype(np.uint32)


def read_indicesweights_mtx(path):
    v = numbers(path, np.float64)
    nnz = int(v[0])
    cw = v[1:1 + 2 * nnz].reshape(nnz, 2)
    return cw[:, 0].astype(np.uint32), cw[:, 1].astype(np.float32)


def read_csr_txt(offsets_path, columns_path):
    o = numbers(offsets_path, np.int64)
    n = int(o[0])
    c = numbers(columns_path, np.int64)
    nnz = int(c[0])
    return n, o[1:1 + n + 1].astype(np.uint32), c[1:1 + nnz].astype(np.uint32)


def find(d, *patterns):
    """The single file in directory d matching one of the glob patterns (first pattern that matches)."""
    for p in patterns:
        m = sorted(glob.glob(os.path.join(d, p)))
        if m:
            return m[0]
    raise SystemExit('*** no %s in %s' % (' / '.join(patterns), d))


def csr_from_edges(n, src, dst):
    """CSR (offsets, columns sorted within each row) of the directed edges src -> dst."""
    order = np.lexsort((dst, src))
    src, dst = src[order], dst[order]
    offsets = np.zeros(n + 1, dtype=np.uint32)
    np.cumsum(np.bincount(src, minlength=n), out=offsets[1:])
    return offsets, dst.astype(np.uint32)


def random_simple_edges(n, m, rng):
    """m distinct undirected edges (u < v) among n vertices, no self-loops."""
    edges = set()
    while len(edges) < m:
        k = int((m - len(edges)) * 1.2) + 16
        u = rng.integers(0, n, k)
        v = rng.integers(0, n, k)
        for a, b in zip(u.tolist(), v.tolist()):
            if a != b:
                edges.add((a, b) if a < b else (b, a))
                if len(edges) == m:
                    break
    e = np.array(sorted(edges), dtype=np.int64)
    return e[:, 0], e[:, 1]


def symmetric_csr(n, m, seed):
    """An undirected random graph stored in both directions (what MIS and label propagation take)."""
    u, v = random_simple_edges(n, m, np.random.default_rng(seed))
    return csr_from_edges(n, np.concatenate([u, v]), np.concatenate([v, u]))


def pad(a, multiple=1024):
    """a, zero-extended to a multiple of `multiple` elements (the hosts round the buffers up the same way)."""
    k = (-len(a)) % multiple
    return np.concatenate([a, np.zeros(k, dtype=a.dtype)]) if k else a
