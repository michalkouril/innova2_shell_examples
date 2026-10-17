#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# aes256CbcDecrypt.py <xclbin> [--len B] [--num N] -- pyxrt host for AMD Vitis Libraries
# security/L1/benchmarks/aes256CbcDecrypt/host/main.cpp (ported, with xf_security/msgpack.hpp's aesCbcPack layout): N
# messages of LEN bytes (AMD's repeating 16-byte pattern), each AES-256-CBC encrypted on the host with AMD's key/IV
# (cryptography; AMD's host reads the same ciphertext from gld.dat), packed as AMD's packer does (row 0: msg_num,
# row count; per message: length row, IV row, 2 key rows, ciphertext), one launch, every decrypted message compared
# with the plaintext. Throughput = N * LEN / kernel time (README: 0.96 GB/s at 32K x 1 KB; single engine).
import argparse, os, sys
import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

p = argparse.ArgumentParser()
p.add_argument('xclbin'); p.add_argument('--len', type=int, default=1024); p.add_argument('--num', type=int, default=32768)
a = p.parse_args()
L, N = a.len, a.num
assert L % 16 == 0, 'length must be a multiple of 16'
key = bytes(range(0x10, 0x30)); iv = bytes(range(0x30, 0x40))
msg = bytes(range(0x60, 0x70)) * (L // 16)
enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
ct = enc.update(msg) + enc.finalize()                      # no padding needed: L is a multiple of 16

rows_msg = 1 + 1 + 2 + L // 16
blkb = np.zeros(rows_msg * 16, np.uint8)
blkb[0:8] = np.frombuffer(np.uint64(L).tobytes(), np.uint8)
blkb[16:32] = np.frombuffer(iv, np.uint8); blkb[32:64] = np.frombuffer(key, np.uint8)
blkb[64:] = np.frombuffer(ct, np.uint8)
inp = np.zeros(16 + rows_msg * 16 * N, np.uint8)
inp[0:16] = np.frombuffer(np.array([N, 1 + rows_msg * N], np.int64).tobytes(), np.uint8)
inp[16:] = np.tile(blkb, N)
out_size = (L + 16) * N + 16

c = Card(a.xclbin); k = c.kernel('aes256CbcDecryptKernel')
ib = c.bo(k, 0, inp.size).put(inp); ob = c.bo(k, 1, out_size).zero()
t = timed_timeout(c, k, ib, ob)
o = ob.get()
res_num = int(o[0:8].view(np.uint64)[0])
recs = o[16:16 + (L + 16) * N].reshape(N, L + 16)
lens = recs[:, 0:8].copy().view(np.uint64).ravel()
bad = int(np.count_nonzero(lens != L)) + int(np.count_nonzero(np.any(recs[:, 16:] != np.frombuffer(msg, np.uint8), axis=1)))
gbs = N * L / t / 1e9
print('aes256CbcDecrypt: %d messages x %d B, res num %d, kernel %.2f ms, %.2f GB/s (%.2f GiB/s); %d wrong'
      % (N, L, res_num, t * 1e3, gbs, N * L / t / 2**30, bad))
sys.exit(result('aes256CbcDecrypt', res_num == N and bad == 0, '%.2f GB/s' % gbs))
