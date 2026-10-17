#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# aes256CbcDecrypt_4pu.py <xclbin> [--tasks T] [--rep R] -- pyxrt host for the overlay benchmark
# security/L1/benchmarks/aes256CbcDecrypt_4pu (AMD Vitis Libraries v2020.2_rel host/main.cpp, ported with our fix:
# the golden is computed without padding). 4 channels x T tasks of 64 x 16-byte blocks (= 1 KB messages): config word
# per channel (row count, task count, IV, key), then the same 16-byte block in all four 128-bit lanes of every word;
# the kernel decrypts each lane's stream (AES-256-CBC); every output lane is compared with the OpenSSL-equivalent
# golden (cryptography). R ping-pong runs as AMD's host (default 2); throughput per run = 4 * T * 1 KB / kernel time
# (README: 3.65 GB/s = 3.40 GiB/s at T = 8192).
import argparse, os, sys
import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

p = argparse.ArgumentParser()
p.add_argument('xclbin'); p.add_argument('--tasks', type=int, default=8192); p.add_argument('--rep', type=int, default=2)
a = p.parse_args()
N_ROW, T, CH = 64, a.tasks, 4
key = bytes(range(0x10, 0x30)); iv = bytes(range(0x30, 0x40)); blk = bytes(range(16))
dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
golden = np.frombuffer(dec.update(blk * N_ROW) + dec.finalize(), np.uint8).reshape(N_ROW, 16)

words = N_ROW * T * CH // 4
inp = np.zeros((CH + words, 64), np.uint8)
cfg = np.zeros(64, np.uint8)
cfg[0:32] = np.frombuffer(key, np.uint8); cfg[32:48] = np.frombuffer(iv, np.uint8)
cfg[48:56] = np.frombuffer(np.uint64(T).tobytes(), np.uint8); cfg[56:64] = np.frombuffer(np.uint64(N_ROW).tobytes(), np.uint8)
inp[:CH] = cfg
inp[CH:] = np.tile(np.frombuffer(blk, np.uint8), 4)
c = Card(a.xclbin); k = c.kernel('aes256CbcDecryptKernel')
ibs = [c.bo(k, 0, inp.size).put(inp) for _ in range(2)]
obs = [c.bo(k, 1, words * 64).zero() for _ in range(2)]
want = np.tile(golden, (T, 4)).reshape(T * N_ROW, 64)      # each word: the same golden block in all four lanes
times, bad = [], 0
for i in range(max(2, a.rep)):
    j = 1 - (i & 1)                                         # AMD's order: run 0 uses buffer b, run 1 buffer a, ...
    times.append(timed_timeout(c, k, ibs[j], obs[j]))
    bad += int(np.count_nonzero(np.any(obs[j].get().reshape(T * N_ROW, 64) != want, axis=1)))
mb = N_ROW * 16 * T * CH
for i, t in enumerate(times):
    print('aes256CbcDecrypt_4pu run %d: %.1f MB in %.2f ms, %.2f GB/s (%.2f GiB/s)'
          % (i, mb / 1e6, t * 1e3, mb / t / 1e9, mb / t / 2**30))
gbs = mb / min(times) / 1e9
print('aes256CbcDecrypt_4pu: %d channels x %d tasks x %d blocks, %d wrong words' % (CH, T, N_ROW, bad))
sys.exit(result('aes256CbcDecrypt_4pu', bad == 0, '%.2f GB/s' % gbs))
