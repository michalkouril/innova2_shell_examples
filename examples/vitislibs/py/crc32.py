#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# crc32.py <xclbin> [--size B] [--num N] [--data FILE] -- pyxrt host for AMD Vitis Libraries
# security/L1/benchmarks/crc32/host/main.cpp
# (ported; same kernel call and check): N messages of one SIZE-byte block (default 8 KB of seeded random bytes, or
# --data), each padded to 16-byte words (the kernel header's W = 16) and laid out back to back; one launch of
# CRC32Kernel(num, words, len[], init[], data, out[]); every message's CRC-32 compared with Python's zlib.
# Throughput = N * SIZE / kernel time (README: 4.67 GB/s at N = 32768, 256 MB).
import argparse, os, sys, zlib
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

p = argparse.ArgumentParser()
p.add_argument('xclbin'); p.add_argument('--size', type=int, default=8192); p.add_argument('--num', type=int, default=32768)
p.add_argument('--data', help='message file (default: SIZE seeded random bytes)')
a = p.parse_args()
msg = open(a.data, 'rb').read() if a.data else np.random.default_rng(20261017).integers(0, 256, a.size, np.uint8).tobytes()
size, num, W = len(msg), a.num, 16                  # W: bytes per data word, as the kernel header's W
words = (size + W - 1) // W                         # data words per message (the kernel's size counts these)
blk = np.zeros(words * W, np.uint8); blk[:size] = np.frombuffer(msg, np.uint8)
c = Card(a.xclbin); k = c.kernel('CRC32Kernel')
lens = c.bo(k, 2, 4 * num).put(np.full(num, size, np.uint32))
init = c.bo(k, 3, 4 * num).put(np.full(num, 0xFFFFFFFF, np.uint32))
data = c.bo(k, 4, words * W * num).put(np.tile(blk, num))
out = c.bo(k, 5, 4 * num).zero()
t = timed_timeout(c, k, num, words * num, lens, init, data, out)
got = out.get(np.uint32); want = zlib.crc32(msg) & 0xFFFFFFFF
bad = int(np.count_nonzero(got != want))
gbs = num * size / t / 1e9
print('crc32: %d x %d B = %.0f MB, kernel %.2f ms, %.2f GB/s; %s 0x%08x, %d wrong'
      % (num, size, num * size / 1e6, t * 1e3, gbs, 'CRC-32', want, bad))
sys.exit(result('crc32', bad == 0, '%.2f GB/s' % gbs))
