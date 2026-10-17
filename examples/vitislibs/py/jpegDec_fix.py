#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# jpegDec_fix.py <xclbin> [--jpeg FILE] [--runs N] [--tol T] -- pyxrt host for the overlay benchmark
# codec/L2/demos/jpegDec_fix (AMD Vitis Libraries codec/L2/demos/jpegDec host/test_decoder.cpp, ported; kernel with our
# copyInfo fix): kernelJpegDecoder(jpeg, size, yuv, infos), N runs (AMD: 10) and the average kernel time (README:
# 0.98 ms for t0.jpg). AMD's host checks the two error codes in infos[4..5]; this port also checks the image
# geometry in infos against the file's SOF and compares every decoded 8x8 block with an independent baseline JPEG
# decode in Python (Huffman, dequantisation, float IDCT, +128, clamp): AMD's IDCT is an integer approximation, so
# samples may differ by a little -- the run fails if any differs by more than --tol (default 2) or the PSNR is below
# 40 dB. Default input: images/t0.jpg of the overlay directory (AMD's test image).
import argparse, os, sys, struct
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

MAXCMP_BC = 1036800                     # kernel output capacity: MAXCMP_BC x 8 words of 64 bits
ZIGZAG = np.array([0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,
                   7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
                   39, 46, 53, 60, 61, 54, 47, 55, 62, 63])


def parse(jpg):
    """Baseline JPEG -> (frame info, list of (component index, 8x8 sample block) in MCU order)."""
    i, q, huff, comps, scan, dri = 2, {}, {}, [], None, 0
    assert jpg[:2] == b'\xff\xd8', 'not a JPEG'
    while i < len(jpg):
        if jpg[i] != 0xFF:
            raise ValueError('marker expected at %d' % i)
        m = jpg[i + 1]; i += 2
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            continue
        ln = struct.unpack('>H', jpg[i:i + 2])[0]; seg = jpg[i + 2:i + ln]
        if m == 0xDB:
            j = 0
            while j < len(seg):
                pq, tq = seg[j] >> 4, seg[j] & 15; j += 1
                n = 128 if pq else 64
                v = np.frombuffer(seg[j:j + n], '>u2' if pq else np.uint8).astype(np.int32); j += n
                t = np.zeros(64, np.int32); t[ZIGZAG] = v; q[tq] = t.reshape(8, 8)
        elif m == 0xC4:
            j = 0
            while j < len(seg):
                tc, th = seg[j] >> 4, seg[j] & 15; cnt = list(seg[j + 1:j + 17]); j += 17
                syms = list(seg[j:j + sum(cnt)]); j += sum(cnt)
                code, k, table = 0, 0, {}
                for L in range(1, 17):
                    for _ in range(cnt[L - 1]):
                        table[(L, code)] = syms[k]; k += 1; code += 1
                    code <<= 1
                huff[(tc, th)] = table
        elif m == 0xC0 or m == 0xC1:
            h, w, nc = struct.unpack('>HHB', seg[1:6])
            comps = [dict(id=seg[6 + 3 * c], h=seg[7 + 3 * c] >> 4, v=seg[7 + 3 * c] & 15, tq=seg[8 + 3 * c])
                     for c in range(nc)]
            frame = dict(w=w, h=h)
        elif m in (0xC2, 0xC3) or 0xC5 <= m <= 0xCF and m not in (0xC8, 0xCC):
            raise ValueError('only baseline JPEG is supported (SOF%d)' % (m - 0xC0))
        elif m == 0xDD:
            dri = struct.unpack('>H', seg[:2])[0]
        elif m == 0xDA:
            ns = seg[0]
            scan = [(seg[1 + 2 * s], seg[2 + 2 * s] >> 4, seg[2 + 2 * s] & 15) for s in range(ns)]
            i += ln
            data = jpg[i:]
            break
        i += ln
    hmax = max(c['h'] for c in comps); vmax = max(c['v'] for c in comps)
    mcux = (frame['w'] + 8 * hmax - 1) // (8 * hmax); mcuy = (frame['h'] + 8 * vmax - 1) // (8 * vmax)
    frame.update(mcux=mcux, mcuy=mcuy, comps=comps)
    # entropy-coded data: un-stuff 0xFF00, split at restart markers
    bits = Bits(data)
    cid = {c['id']: n for n, c in enumerate(comps)}
    order = [(cid[s[0]], s[1], s[2]) for s in scan]
    pred = [0] * len(comps); blocks = []
    C = np.array([[(np.sqrt(1 / 8) if u == 0 else np.sqrt(2 / 8)) * np.cos((2 * x + 1) * u * np.pi / 16)
                   for x in range(8)] for u in range(8)])
    for mcu in range(mcux * mcuy):
        if dri and mcu and mcu % dri == 0:
            bits.restart(); pred = [0] * len(comps)
        for ci, td, ta in order:
            c = comps[ci]
            for _ in range(c['h'] * c['v']):
                coef = np.zeros(64, np.int32)
                t = bits.decode(huff[(0, td)]); diff = bits.receive(t)
                pred[ci] += diff; coef[0] = pred[ci]
                k = 1
                while k < 64:
                    rs = bits.decode(huff[(1, ta)]); r, s = rs >> 4, rs & 15
                    if s == 0:
                        if r == 15:
                            k += 16; continue
                        break
                    k += r; coef[ZIGZAG[k]] = bits.receive(s); k += 1
                d = coef.reshape(8, 8) * q[c['tq']]
                blk = C.T @ d @ C + 128
                blocks.append((ci, np.clip(np.round(blk), 0, 255).astype(np.uint8)))
    return frame, blocks


class Bits:
    def __init__(self, d):
        self.d, self.p, self.acc, self.n = d, 0, 0, 0

    def _fill(self):
        while self.n <= 24:
            b = self.d[self.p] if self.p < len(self.d) else 0
            if b == 0xFF:
                nx = self.d[self.p + 1] if self.p + 1 < len(self.d) else 0
                if nx == 0:
                    self.p += 2
                elif 0xD0 <= nx <= 0xD7 or nx == 0xD9:
                    b = 0                       # marker: feed zeros, do not advance
                else:
                    self.p += 1
            else:
                self.p += 1
            self.acc = (self.acc << 8) | b; self.n += 8

    def bit(self):
        if self.n == 0:
            self._fill()
        self.n -= 1
        return (self.acc >> self.n) & 1

    def decode(self, table):
        code = 0
        for L in range(1, 17):
            code = (code << 1) | self.bit()
            v = table.get((L, code))
            if v is not None:
                return v
        raise ValueError('bad Huffman code')

    def receive(self, s):
        v = 0
        for _ in range(s):
            v = (v << 1) | self.bit()
        return v - (1 << s) + 1 if s and v < (1 << (s - 1)) else v

    def restart(self):
        self.acc, self.n = 0, 0
        while self.p + 1 < len(self.d) and not (self.d[self.p] == 0xFF and 0xD0 <= self.d[self.p + 1] <= 0xD7):
            self.p += 1
        self.p += 2


p = argparse.ArgumentParser()
p.add_argument('xclbin')
p.add_argument('--jpeg', default=os.path.join(here, '..', 'overlay', 'codec', 'L2', 'demos', 'jpegDec_fix', 'images', 't0.jpg'))
p.add_argument('--runs', type=int, default=10); p.add_argument('--tol', type=int, default=2)
a = p.parse_args()
jpg = open(a.jpeg, 'rb').read()
frame, ref = parse(jpg)
c = Card(a.xclbin); k = c.kernel('kernelJpegDecoder')
ib = c.bo(k, 0, len(jpg)).put(np.frombuffer(jpg, np.uint8))
yb = c.bo(k, 2, MAXCMP_BC * 8 * 8); fb = c.bo(k, 3, 1024 * 4).zero()
times = [timed_timeout(c, k, ib, len(jpg), yb, fb) for _ in range(a.runs)]
info = fb.get(np.uint32)
nblk = int(info[10]); rtn, rtn2 = int(info[4]), int(info[5])
out = yb.get(np.uint8, nblk * 64).reshape(nblk, 8, 8)       # per block: 8 words = 8 columns of 8 samples
got = out.transpose(0, 2, 1)                                # -> [block][row][col]
want = np.stack([b for _, b in ref]) if ref else np.zeros((0, 8, 8), np.uint8)
geo_ok = (nblk == len(ref) and int(info[1]) == frame['mcux'] * frame['mcuy'] and int(info[0]) == len(frame['comps']))
if nblk == len(ref):
    d = got.astype(np.int16) - want
    mx = int(np.abs(d).max()); mse = float(np.mean(d.astype(np.float64) ** 2))
    psnr = 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)
else:
    mx, psnr = 999, 0.0
ok = rtn == 0 and rtn2 == 0 and geo_ok and mx <= a.tol and psnr >= 40
print('jpegDec_fix: %s %dx%d, %d components, %d MCUs, %d blocks; error codes %d/%d; kernel %.3f ms average of %d runs; '
      'vs the Python decode: max |diff| %d, PSNR %.1f dB%s'
      % (os.path.basename(a.jpeg), frame['w'], frame['h'], len(frame['comps']), int(info[1]), nblk, rtn, rtn2,
         1e3 * sum(times) / len(times), len(times), mx, psnr,
         '' if geo_ok else ' -- GEOMETRY MISMATCH (kernel %d blocks, file %d)' % (nblk, len(ref))))
sys.exit(result('jpegDec_fix', ok, '%.2f ms, max diff %d, %.1f dB' % (1e3 * sum(times) / len(times), mx, psnr)))
