# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""zuc_model -- ../e5/zuc/zuc_model.h in Python: ZUC-128 (ETSI/SAGE spec v1.6 / 3GPP TS 35.221) and the 128-EEA3 /
128-EIA3 constructions (TS 35.222), for the expected replies of e5gen_host.py --zuc. The S-boxes are read from
../e5/zuc/zuc_sbox.h (generated from flexdriver-zuc's .coe files, BSD-2-Clause, (c) 2020-2021 Gabi Malka), so the
tables are the ones the C host and the RTL use. selftest() checks the spec's keystream test vectors.
"""
import os
import re

_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'e5', 'zuc', 'zuc_sbox.h')
_src = open(_H).read()
S0, S1 = ([int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', _src.split(n)[1].split('};')[0])] for n in ('ZUC_S0', 'ZUC_S1'))
assert len(S0) == 256 and len(S1) == 256
M31, M32 = 0x7fffffff, 0xffffffff
D = (0x44D7, 0x26BC, 0x626B, 0x135E, 0x5789, 0x35E2, 0x7135, 0x09AF, 0x4D78, 0x2F13, 0x6BC4, 0x1AF1, 0x5E26, 0x3C4D, 0x789A, 0x47AC)


def _rot32(a, k):
    return ((a << k) | (a >> (32 - k))) & M32


def _l1(x):
    return x ^ _rot32(x, 2) ^ _rot32(x, 10) ^ _rot32(x, 18) ^ _rot32(x, 24)


def _l2(x):
    return x ^ _rot32(x, 8) ^ _rot32(x, 14) ^ _rot32(x, 22) ^ _rot32(x, 30)


def _sbox(x):
    return (S0[x >> 24] << 24) | (S1[(x >> 16) & 0xff] << 16) | (S0[(x >> 8) & 0xff] << 8) | S1[x & 0xff]


def _add31(a, b):
    c = a + b
    return (c & M31) + (c >> 31)


def _rot31(a, k):
    return ((a << k) | (a >> (31 - k))) & M31


class Zuc:
    def __init__(self, key, iv):
        self.s = [(key[i] << 23) | (D[i] << 8) | iv[i] for i in range(16)]
        self.r1 = self.r2 = 0
        for _ in range(32):
            self._br()
            w = self._f()
            self._lfsr(w >> 1)
        self._br()
        self._f()
        self._lfsr(0)                                  # first working-mode step, output discarded

    def _br(self):
        s = self.s
        self.x0 = ((s[15] & 0x7fff8000) << 1) | (s[14] & 0xffff)
        self.x1 = ((s[11] & 0xffff) << 16) | (s[9] >> 15)
        self.x2 = ((s[7] & 0xffff) << 16) | (s[5] >> 15)
        self.x3 = ((s[2] & 0xffff) << 16) | (s[0] >> 15)

    def _f(self):
        w = ((self.x0 ^ self.r1) + self.r2) & M32
        w1, w2 = (self.r1 + self.x1) & M32, self.r2 ^ self.x2
        self.r1 = _sbox(_l1(((w1 << 16) | (w2 >> 16)) & M32))
        self.r2 = _sbox(_l2(((w2 << 16) | (w1 >> 16)) & M32))
        return w

    def _lfsr(self, u):
        s = self.s
        f = s[0]
        for i, k in ((0, 8), (4, 20), (10, 21), (13, 17), (15, 15)):
            f = _add31(f, _rot31(s[i], k))
        if u:
            f = _add31(f, u)
        if f == 0:
            f = M31
        del s[0]
        s.append(f)

    def word(self):
        self._br()
        k = self._f() ^ self.x3
        self._lfsr(0)
        return k


def keystream(key, iv, n):
    z = Zuc(key, iv)
    return [z.word() for _ in range(n)]


def zuc_xor(key, iv, data):
    """128-EEA3 core: data XOR the keystream (word i covers bytes 4i..4i+3, most significant byte first)."""
    ks = keystream(key, iv, (len(data) + 3) // 4)
    k = b''.join(w.to_bytes(4, 'big') for w in ks)
    return bytes(a ^ b for a, b in zip(data, k))


def eia3_mac(key, iv, msg, bits):
    """128-EIA3 MAC over `bits` message bits (message bytes MSB-first)."""
    L = (bits + 31) // 32 + 2
    ks = keystream(key, iv, L) + [0]
    big = 0
    for w in ks:
        big = (big << 32) | w
    tot = 32 * len(ks)

    def kw(i):                                         # the 32-bit keystream word starting at bit i
        return (big >> (tot - 32 - i)) & M32
    m = int.from_bytes(msg[:(bits + 7) // 8], 'big') if bits else 0
    nb = 8 * ((bits + 7) // 8)
    t = 0
    for i in range(bits):
        if (m >> (nb - 1 - i)) & 1:
            t ^= kw(i)
    t ^= kw(bits)
    return t ^ kw(32 * (L - 1))


def selftest():
    if keystream(bytes(16), bytes(16), 2) != [0x27bede74, 0x018082da]:
        return 1
    if keystream(b'\xff' * 16, b'\xff' * 16, 2) != [0x0657cfa0, 0x7096398b]:
        return 2
    return 0


if __name__ == '__main__':
    print('ZUC self-test', 'PASS' if selftest() == 0 else 'FAIL')
