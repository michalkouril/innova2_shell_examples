#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""make_vectors.py <out> <n> [random] -- test jobs for rsa_modexp: one line per job, 6 hex numbers:
base exp mod r2 n0p expected. Default: n/2 RSA-2048 signatures (4 keys, random messages), each as its two CRT halves
(s_p = m^dp mod p, s_q = m^dq mod q), and every recombined signature is verified here (s^e = m mod n).
'random': the first two jobs as above, the rest random 1024-bit moduli and exponents."""
import random, sys
r = random.Random(7)
def prime(bits):
    while True:
        c = r.getrandbits(bits) | (1 << (bits - 1)) | 1
        if all(pow(a, c - 1, c) == 1 for a in (2, 3, 5, 7, 11, 13)): return c
def job(b, e, m):
    return [b, e, m, pow(2, 2048, m), (-pow(m, -1, 1 << 64)) % (1 << 64), pow(b, e, m)]
jobs = []
p, q = prime(1024), prime(1024); n = p * q; e = 65537; d = pow(e, -1, (p - 1) * (q - 1))
msg = r.getrandbits(2047) % n
dp, dq, qinv = d % (p - 1), d % (q - 1), pow(q, -1, p)
jobs += [job(msg % p, dp, p), job(msg % q, dq, q)]
sp, sq = jobs[0][5], jobs[1][5]; s = sq + ((qinv * (sp - sq)) % p) * q
assert s == pow(msg, d, n) and pow(s, e, n) == msg
if len(sys.argv) > 3 and sys.argv[3] == 'random':
    for _ in range(int(sys.argv[2]) - 2):
        m = r.getrandbits(1024) | (1 << 1023) | 1
        jobs.append(job(r.getrandbits(1024) % m, r.getrandbits(1024), m))
else:
    keys = []
    for _ in range(4):
        p, q = prime(1024), prime(1024); n = p * q; d = pow(e, -1, (p - 1) * (q - 1))
        keys.append((p, q, n, d % (p - 1), d % (q - 1), pow(q, -1, p)))
    while len(jobs) < int(sys.argv[2]):
        p, q, n, dp, dq, qinv = keys[len(jobs) // 2 % 4]
        msg = r.getrandbits(2047) % n
        a, b = job(msg % p, dp, p), job(msg % q, dq, q)
        s = b[5] + ((qinv * (a[5] - b[5])) % p) * q
        assert pow(s, e, n) == msg
        jobs += [a, b]
with open(sys.argv[1], 'w') as f:
    for j in jobs: f.write(' '.join('%x' % v for v in j) + '\n')
print('%d jobs written' % len(jobs))
