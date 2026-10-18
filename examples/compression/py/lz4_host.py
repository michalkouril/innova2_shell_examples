#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# lz4_host.py <xclbin> <input> -- pyxrt host for AMD Vitis Libraries data_compression xilLz4Compress
# (L2/src/lz4_compress_mm.cpp), ported from common/libs/compress/lz4OCLHost.cpp (compressEngine) and lz4Base.cpp: the
# input in 32 MB host chunks, each cut into 64 KB blocks; one launch per chunk of
# xilLz4Compress(in, out, compressd_size[], in_block_size[], 64, chunk bytes); every block is written as the kernel's
# compressed block or, when that is not smaller, stored raw. Writes <input>.lz4, an LZ4 frame that the stock lz4 -d
# expands. One difference from the AMD host: the frame carries no content checksum (FLG 0x60, not 0x64), because a
# pure-Python xxHash32 of the whole input would take longer than the compression; lz4 -d still checks every block.
# Prints the kernel throughput (input bytes over the summed kernel time) as "KT(MBps) : <x>", as the AMD host.
# The same host serves lz4, lz4x4 and lz4x8 (PARALLEL_BLOCK only changes the kernel's internal parallelism).
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout  # noqa: E402

HOST_BUFFER_SIZE, BLOCK_KB = 32 << 20, 64
P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
M = 0xFFFFFFFF


def xxh32_small(b, seed=0):
    """xxHash32 of a short (< 16 byte) input: the LZ4 frame descriptor checksum."""
    assert len(b) < 16
    h = (seed + P5 + len(b)) & M
    i = 0
    while i + 4 <= len(b):
        h = (h + int.from_bytes(b[i:i + 4], 'little') * P3) & M
        h = (((h << 17) | (h >> 15)) & M) * P4 & M
        i += 4
    for c in b[i:]:
        h = (h + c * P5) & M
        h = (((h << 11) | (h >> 21)) & M) * P1 & M
    h ^= h >> 15
    h = (h * P2) & M
    h ^= h >> 13
    h = (h * P3) & M
    return h ^ (h >> 16)


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: %s <xclbin> <input>' % sys.argv[0])
    data = np.fromfile(sys.argv[2], np.uint8)
    n = data.size
    if n == 0:
        sys.exit('*** empty input')
    c = Card(sys.argv[1])
    k = c.kernel('xilLz4Compress')
    bsz = BLOCK_KB * 1024
    chunk = min(HOST_BUFFER_SIZE, max(n, bsz))
    nb_max = (chunk - 1) // bsz + 1
    bin_, bout = c.bo(k, 0, chunk), c.bo(k, 1, chunk)
    bcs, bbs = c.bo(k, 2, 4 * nb_max), c.bo(k, 3, 4 * nb_max)
    flg, bd = 0x60, 0x40                                  # version 01, independent blocks; 64 KB blocks
    out = [b'\x04\x22\x4d\x18', bytes((flg, bd, (xxh32_small(bytes((flg, bd))) >> 8) & 0xFF))]
    kt = 0.0
    for off in range(0, n, chunk):
        cn = min(chunk, n - off)
        nb = (cn - 1) // bsz + 1
        sizes = np.full(nb, bsz, np.uint32)
        sizes[-1] = cn - (nb - 1) * bsz
        bin_.put(data[off:off + cn])
        bbs.put(sizes)
        kt += timed_timeout(c, k, bin_, bout, bcs, bbs, BLOCK_KB, cn)
        cs = bcs.get(np.uint32, nb)
        comp = bout.get(np.uint8, None)
        for b in range(nb):
            bl, sz = int(sizes[b]), int(cs[b])
            if sz == 0:
                sys.exit('*** block %d: the kernel reported 0 bytes' % b)
            if sz < bl:
                out.append(sz.to_bytes(4, 'little'))
                out.append(comp[b * bsz:b * bsz + sz].tobytes())
            else:                                         # no gain: store the block raw
                out.append((bl | 0x80000000).to_bytes(4, 'little'))
                out.append(data[off + b * bsz:off + b * bsz + bl].tobytes())
    out.append(bytes(4))                                  # end mark
    blob = b''.join(out)
    open(sys.argv[2] + '.lz4', 'wb').write(blob)
    print('LZ4: %d bytes -> %d (CR %.2f)' % (n, len(blob), n / len(blob)))
    print('KT(MBps) : %.2f' % (n / kt / 1e6))
    return result('lz4', True, '%d -> %d bytes' % (n, len(blob)))


if __name__ == '__main__':
    sys.exit(main())
