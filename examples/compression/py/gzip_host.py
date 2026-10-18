#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# gzip_host.py <xclbin> <input> [cus] -- pyxrt host for AMD Vitis Libraries data_compression xilGzipCompBlock
# (L2/src/gzip_compress_multicore_mm.cpp): the Python twin of ../host/gzip_xn_host.cpp, which itself follows the
# stock host's framing (common/libs/compress/gzipOCLHost.cpp compressEngineSeq, gzipBase.cpp). The input is cut into
# one part per compute unit (whole 32 KB blocks; cus = 1 for gzip / gzip8 / gzip_combo, 4 for gzip_x4); each CU
# compresses its part in one launch of xilGzipCompBlock(in, out, compressd_size[], checksum, bytes, true = CRC32)
# into a complete gzip member (12-byte header, the kernel's deflate blocks, a final empty stored block, CRC32 and
# ISIZE); the members are concatenated into <input>.gz, which gzip -d expands into the original (RFC 1952 allows
# several members). Prints the kernel throughput (input bytes over first start .. last completion) as
# "KT(MBps) :<x>", and the per-CU times.
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, arg32, result, start_args, wait_ok  # noqa: E402

BLOCK = 32 * 1024                                         # the kernel's deflate block size (BLOCKSIZE_IN_KB)


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: %s <xclbin> <input> [cus=1]' % sys.argv[0])
    data = np.fromfile(sys.argv[2], np.uint8)
    n = data.size
    ncu = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    if n == 0 or n >= 1 << 32:
        sys.exit('*** input must be 1 byte .. 4 GB')
    c = Card(sys.argv[1])
    nblk = (n + BLOCK - 1) // BLOCK
    np_ = min(ncu, nblk)
    parts, b0 = [], 0
    for i in range(np_):
        b1 = nblk * (i + 1) // np_
        off, ln = b0 * BLOCK, min(n, b1 * BLOCK) - b0 * BLOCK
        b0 = b1
        k = c.kernel('xilGzipCompBlock:{xilGzipCompBlock_%d}' % (i + 1))
        nb = (ln + BLOCK - 1) // BLOCK
        p = dict(off=off, len=ln, k=k, bi=c.bo(k, 0, ln), bo=c.bo(k, 1, ln + ln // 8 + 64 * 1024),
                 bs=c.bo(k, 2, 2 * nb * 4), bc=c.bo(k, 3, 4))
        p['bi'].put(data[off:off + ln])
        p['bc'].put(np.array([0xFFFFFFFF], np.uint32))
        parts.append(p)
    t0 = time.perf_counter()
    for p in parts:
        p['r'] = start_args(p['k'], {0: p['bi'], 1: p['bo'], 2: p['bs'], 3: p['bc'], 4: arg32(p['len']), 5: 1})
    for p in parts:
        if not wait_ok(p['r'], 300):
            return result('gzip', False, 'CU did not complete')
        p['ms'] = (time.perf_counter() - t0) * 1e3
    ms = (time.perf_counter() - t0) * 1e3
    out = []
    hdr = bytes((0x1f, 0x8b, 0x08, 0x08, 0, 0, 0, 0, 0x00, 0x03, ord('x'), 0))   # the stock host's header
    last = bytes((0x01, 0x00, 0x00, 0xff, 0xff))          # final empty stored block
    for p in parts:
        sz = int(p['bs'].get(np.uint32, 1)[0])
        crc = int(p['bc'].get(np.uint32, 1)[0])
        if sz == 0 or sz > p['bo'].size:
            return result('gzip', False, 'CU reported %u compressed bytes' % sz)
        out += [hdr, p['bo'].get(np.uint8, sz).tobytes(), last, crc.to_bytes(4, 'little'), (p['len'] & 0xFFFFFFFF).to_bytes(4, 'little')]
    blob = b''.join(out)
    open(sys.argv[2] + '.gz', 'wb').write(blob)
    print('GZIP-PY: %d CUs, %d bytes -> %d (CR %.2f), %.2f ms, KT(MBps) :%.2f' % (np_, n, len(blob), n / len(blob), ms, n / ms / 1000.0))
    for i, p in enumerate(parts):
        print('  CU %d: %d bytes, done at %.2f ms' % (i + 1, p['len'], p['ms']))
    return result('gzip', True, '%d -> %d bytes' % (n, len(blob)))


if __name__ == '__main__':
    sys.exit(main())
