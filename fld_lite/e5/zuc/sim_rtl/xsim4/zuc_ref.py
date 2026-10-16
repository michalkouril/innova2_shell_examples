#!/usr/bin/env python3
"""Standard ZUC-128 (v1.6) + 128-EEA3/128-EIA3 reference, written from the 3GPP spec.
S-boxes are parsed from the FLD repo's .coe files so the tables are the ones the RTL uses."""
import re, os
SRC = os.environ.get('FLEXDRIVER_ZUC_SRC', 'flexdriver-zuc/src')   # .coe S-box files of the flexdriver-zuc repository
def load_coe(p):
    t = open(p).read().split('memory_initialization_vector')[1]
    return [int(x, 16) for x in re.findall(r'[0-9a-fA-F]{2}', t.split('=',1)[1])]
S0 = load_coe(os.path.join(SRC, 's0_box.coe')); S1 = load_coe(os.path.join(SRC, 's1_box.coe'))
assert len(S0) == 256 and len(S1) == 256
D = [0x44D7,0x26BC,0x626B,0x135E,0x5789,0x35E2,0x7135,0x09AF,0x4D78,0x2F13,0x6BC4,0x1AF1,0x5E26,0x3C4D,0x789A,0x47AC]
M31 = 0x7FFFFFFF; M32 = 0xFFFFFFFF
def rot32(x, k): return ((x << k) | (x >> (32 - k))) & M32
def L1(x): return x ^ rot32(x,2) ^ rot32(x,10) ^ rot32(x,18) ^ rot32(x,24)
def L2(x): return x ^ rot32(x,8) ^ rot32(x,14) ^ rot32(x,22) ^ rot32(x,30)
def addm(a,b):
    c = a + b; return (c & M31) + (c >> 31)
def mulp2(x,k): return ((x << k) | (x >> (31 - k))) & M31
def S(x): return (S0[x>>24]<<24)|(S1[(x>>16)&255]<<16)|(S0[(x>>8)&255]<<8)|S1[x&255]
class ZUC:
    def __init__(self, k, iv, trace=None):
        self.s = [(k[i]<<23)|(D[i]<<8)|iv[i] for i in range(16)]
        self.r1 = self.r2 = 0
        for rnd in range(32):
            self.br(); w = self.f()
            if trace is not None: trace.append(list(self.s))
            self.lfsr(w >> 1)
        self.br(); self.f(); self.lfsr(None)
    def br(self):
        s = self.s
        self.x0 = ((s[15] & 0x7FFF8000) << 1) | (s[14] & 0xFFFF)
        self.x1 = ((s[11] & 0xFFFF) << 16) | (s[9] >> 15)
        self.x2 = ((s[7] & 0xFFFF) << 16) | (s[5] >> 15)
        self.x3 = ((s[2] & 0xFFFF) << 16) | (s[0] >> 15)
    def f(self):
        w = ((self.x0 ^ self.r1) + self.r2) & M32
        w1 = (self.r1 + self.x1) & M32; w2 = self.r2 ^ self.x2
        u = L1(((w1 << 16) | (w2 >> 16)) & M32); v = L2(((w2 << 16) | (w1 >> 16)) & M32)
        self.r1 = S(u); self.r2 = S(v); return w
    def lfsr(self, u):
        s = self.s; f = s[0]
        for x in (mulp2(s[0],8), mulp2(s[4],20), mulp2(s[10],21), mulp2(s[13],17), mulp2(s[15],15)): f = addm(f, x)
        if u is not None: f = addm(f, u)
        if f == 0: f = M31
        self.s = s[1:] + [f]
    def words(self, n):
        out = []
        for _ in range(n):
            self.br(); out.append(self.f() ^ self.x3); self.lfsr(None)
        return out
def eea3_iv(count, bearer, direction):
    c = count.to_bytes(4, 'big'); iv = list(c) + [((bearer << 3) | (direction << 2)) & 0xFC, 0, 0, 0]
    return iv + iv
def eia3_iv(count, bearer, direction):
    c = list(count.to_bytes(4, 'big'))
    iv = c + [(bearer << 3) & 0xF8, 0, 0, 0]
    iv += [c[0] ^ ((direction & 1) << 7)] + c[1:] + [iv[4], 0, (direction & 1) << 7, 0]
    return iv
def eea3(key, count, bearer, direction, msg_bits, lenbits):
    L = (lenbits + 31) // 32
    z = ZUC(list(key), eea3_iv(count, bearer, direction)).words(L)
    return [m ^ k for m, k in zip(msg_bits, z)]
def get_word(z, i):
    j, b = divmod(i, 32)
    if b == 0: return z[j]
    return ((z[j] << b) | (z[j+1] >> (32 - b))) & M32
def eia3_raw(key, iv, msg_bytes, lenbits):
    L = (lenbits + 31) // 32 + 2
    z = ZUC(list(key), list(iv)).words(L)
    T = 0
    for i in range(lenbits):
        if (msg_bytes[i // 8] >> (7 - i % 8)) & 1: T ^= get_word(z, i)
    T ^= get_word(z, lenbits)
    return T ^ z[L - 1]
def eia3(key, count, bearer, direction, msg_bytes, lenbits):
    return eia3_raw(key, eia3_iv(count, bearer, direction), msg_bytes, lenbits)
if __name__ == '__main__':
    # EEA3 test set 1 (as echoed in zuc_core.v:591-600)
    key = bytes.fromhex('173d14ba5003731d7a60049470f00a29')
    iv = eea3_iv(0x66035492, 0xf, 0)
    print('EEA3 TS1 IV', bytes(iv).hex())
    tr = []
    z = ZUC(list(key), iv, tr).words(6)
    print('EEA3 TS1 keystream', ' '.join('%08x' % w for w in z))
    s = tr[1]  # state at the 2nd LFSRinit iteration
    print('iter2 operands: s0=%08x s0p8=%08x s4p20=%08x s10p21=%08x s13p17=%08x s15p15=%08x' % (
        s[0], mulp2(s[0],8), mulp2(s[4],20), mulp2(s[10],21), mulp2(s[13],17), mulp2(s[15],15)))
    # EIA3 test set 1 & 2 (3GPP)
    print('EIA3 TS1 MAC %08x (spec c8a9595e)' % eia3(bytes(16), 0, 0, 0, bytes(4), 1))
    ik = bytes.fromhex('47054125561eb2dda94059da05097850')
    print('EIA3 TS2 MAC %08x (spec 6719a088)' % eia3(ik, 0x561eb2dd, 0x14, 0, bytes(12), 90))
