#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""rsa_host.py <xclbin> [signatures] [rounds] -- RSA-2048 signing on the card's rsa_modexp CU (pyxrt, no build step).

Makes 4 RSA-2048 keys and `signatures` random messages, splits each signature into its two CRT halves
(m^dp mod p, m^dq mod q: 1024-bit modular exponentiations), runs all halves on the card `rounds` times, recombines
every signature on the host and verifies it (s^e mod n == m), and reports signatures/s, the time of one group alone
(2 signatures on one engine: the latency of a signature), and how long Python's own pow() takes per signature.
Needs XRT's Python bindings (PYTHONPATH=/opt/xilinx/xrt/python) and numpy."""
import random, sys, time
import numpy as np
import pyxrt

L = 4                                          # jobs per engine group (the kernel's RSA_L)
xclbin, nsig = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1100
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 10
nsig -= nsig % 2                               # whole groups: 2 signatures = 4 jobs
r = random.Random(11)

def prime(bits):
    while True:
        c = r.getrandbits(bits) | (1 << (bits - 1)) | 1
        if all(pow(a, c - 1, c) == 1 for a in (2, 3, 5, 7, 11, 13, 17, 19)): return c

def limbs(v):                                  # 1024-bit int -> 16 little-endian uint64
    return np.frombuffer(v.to_bytes(128, 'little'), dtype=np.uint64)

E = 65537
keys = []
while len(keys) < 4:
    p, q = prime(1024), prime(1024)
    if p == q or (p - 1) % E == 0 or (q - 1) % E == 0: continue
    d = pow(E, -1, (p - 1) * (q - 1))
    keys.append((p, q, p * q, d % (p - 1), d % (q - 1), pow(q, -1, p)))
sigs = []                                      # (key, message)
for k in range(nsig):
    key = keys[k % 4]; sigs.append((key, r.getrandbits(2047) % key[2]))

# job words: base, exponent, modulus, 2^2048 mod modulus, n0' (low limb), padding -- 10 x 512-bit beats per job
jobs = np.zeros((nsig * 2, 80), dtype=np.uint64)
for k, ((p, q, n, dp, dq, qinv), m) in enumerate(sigs):
    for h, (mod, e) in enumerate(((p, dp), (q, dq))):
        j = jobs[2 * k + h]
        j[0:16] = limbs(m % mod); j[16:32] = limbs(e); j[32:48] = limbs(mod); j[48:64] = limbs(pow(2, 2048, mod))
        j[64] = (-pow(mod, -1, 1 << 64)) % (1 << 64)

dev = pyxrt.device(0)
ctx = pyxrt.hw_context(dev, dev.register_xclbin(pyxrt.xclbin(xclbin)))
krn = pyxrt.kernel(ctx, "rsa_modexp")
nj = jobs.shape[0]
bi = pyxrt.bo(dev, nj * 640, pyxrt.bo.normal, krn.group_id(0))
bo = pyxrt.bo(dev, nj * 128, pyxrt.bo.normal, krn.group_id(1))
bi.write(jobs.tobytes(), 0); bi.sync(pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_TO_DEVICE)

krn(bi, bo, nj).wait()                         # warm-up (and the results checked below)
t0 = time.perf_counter()
for _ in range(rounds): krn(bi, bo, nj).wait()
el = time.perf_counter() - t0
bo.sync(pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_FROM_DEVICE)
res = np.frombuffer(bytes(bo.read(nj * 128, 0)), dtype=np.uint64).reshape(nj, 16)

bad = 0
for k, ((p, q, n, dp, dq, qinv), m) in enumerate(sigs):
    sp = int.from_bytes(res[2 * k].tobytes(), 'little'); sq = int.from_bytes(res[2 * k + 1].tobytes(), 'little')
    s = sq + ((qinv * (sp - sq)) % p) * q      # CRT recombination
    if pow(s, E, n) != m: bad += 1

t1 = time.perf_counter(); krn(bi, bo, L).wait(); lat = (time.perf_counter() - t1) * 1e3      # one group alone
t2 = time.perf_counter()
for (p, q, n, dp, dq, qinv), m in sigs[:50]: pow(m % p, dp, p); pow(m % q, dq, q)
py_ms = (time.perf_counter() - t2) / 50 * 1e3
print("RSA-2048 sign on the card: %d signatures x %d rounds in %.3f s = %.0f signatures/s; one group alone (2 signatures, "
      "one engine) %.2f ms; Python pow() %.2f ms per signature; %d of %d signatures failed verification"
      % (nsig, rounds, el, nsig * rounds / el, lat, py_ms, bad, nsig))
print("RSA-HOST PASS" if bad == 0 else "RSA-HOST FAIL")
sys.exit(1 if bad else 0)
