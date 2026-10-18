#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# gunzip_host.py <xclbin> <input.gz> [mcr] [iters] -- pyxrt host for AMD Vitis Libraries data_compression
# xilDecompress (L2/src/gzip_decompress_mm.cpp, GZIP_MODE), ported from common/libs/compress/gzipOCLHost.cpp
# decompressEngineMMSeq: the whole .gz file in one buffer, an output buffer of input x mcr bytes (default 20, as the
# stock host's -mcr), xilDecompress(in, out, size, input bytes) run `iters` times (the stock host runs it 100 times
# "to get pure IP throughput"; default 10 here), the decompressed size read back. Writes <input.gz>.orig and prints
# the throughput (output bytes x iters over the kernel time) as "KT(MBps) : <x>".
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, arg32, result, wait_ok  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: %s <xclbin> <input.gz> [mcr=20] [iters=10]' % sys.argv[0])
    data = np.fromfile(sys.argv[2], np.uint8)
    mcr = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    iters = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    n = data.size
    osz = min(n * mcr, (1 << 30) - 2)                    # the stock host's 1 GB (4 GB in its comment) output limit
    c = Card(sys.argv[1])
    k = c.kernel('xilDecompress:{xilDecompress_1}')
    bi, bo, bs = c.bo(k, 0, n), c.bo(k, 1, osz), c.bo(k, 2, 8)
    bi.put(data)
    t0 = time.perf_counter()
    runs = [k(bi.bo, bo.bo, bs.bo, arg32(n)) for _ in range(iters)]
    for r in runs:
        if not wait_ok(r, 300):
            return result('gunzip', False, 'kernel did not complete')
    kt = time.perf_counter() - t0
    m = int(bs.get(np.uint32, 1)[0])
    if m == 0 or m > osz:
        return result('gunzip', False, 'decompressed size %d (output buffer %d bytes)' % (m, osz))
    bo.get(np.uint8, m).tofile(sys.argv[2] + '.orig')
    print('GUNZIP: %d bytes -> %d' % (n, m))
    print('KT(MBps) : %.2f' % (m * iters / kt / 1e6))
    return result('gunzip', True, '%d -> %d bytes' % (n, m))


if __name__ == '__main__':
    sys.exit(main())
