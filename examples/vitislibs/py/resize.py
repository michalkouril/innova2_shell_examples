#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# resize.py <xclbin> [--srcw W --srch H] [--sizes WxH,...] [--input RAW] -- pyxrt host for AMD Vitis Libraries
# codec/L2/demos/resize (host/test_resize.cpp, ported; 8 pixels per 64-bit word as its NPPC = 8 build), with the check
# AMD's host lacks (it never sets nerror): every output pixel is compared with a bit-exact numpy model of the kernel,
# AMD's codec/L2/include/hw/resize/bicubicinterpolator.hpp resizeDown_opt_8x in ap_fixed<33,14> arithmetic (19
# fractional bits, each product truncated toward -inf, the ratios as integer divisions, the vertical window = source
# rows r-3..r clamped at the edges, an output row whenever floor(r * ratio) changes, the same horizontally, the result
# converted to 8 bits toward zero). Default input: the 8-bit 7680x4320 test image of check_resize.sh (same seed),
# resized to 512x512, 1920x1080 and 3840x2160 (README: 15.5 ms each, output equal to AMD's C model).
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result, timed_timeout

F = 19; ONE = 1 << F; MASK33 = (1 << 33) - 1


def wrap(v):                              # ap_fixed<33,14> overflow: wrap
    return ((v + (1 << 32)) & MASK33) - (1 << 32)


def fmul(a, b):                           # FMul: product truncated to 19 fractional bits (AP_TRN = toward -inf)
    return wrap((a * b) >> F)


def cubic(w0, w1, w2, w3, x):             # cubicInterpolate_one, term by term
    a, b, c, d, e = ONE // 2, 2 * ONE, 3 * ONE, 4 * ONE, 5 * ONE
    tmpC = fmul(c, wrap(w1 - w2)); tmpB = fmul(b, w0); tmpE = fmul(e, w1); tmpD = fmul(d, w2); tmpA = fmul(a, x)
    tmp1 = fmul(x, wrap(tmpC + w3 - w0))
    tmp2 = fmul(x, wrap(tmpB - tmpE + tmpD - w3 + tmp1))
    return wrap(w1 + fmul(tmpA, wrap(w2 - w0 + tmp2)))


def model(img, dw, dh):
    """The kernel's output for img (uint8, H x W) resized to dw x dh."""
    sh, sw = img.shape
    rows, cols = max(sh, dh), max(sw, dw)
    rr = (dh << F) // sh; cr = (dw << F) // sw            # row / col ratio (fixed_t)
    rrec = (1 << (2 * F)) // rr; crec = (1 << (2 * F)) // cr
    P = img.astype(np.int64) << F
    drow = lambda r: (r * rr) >> F
    # output columns: rel_col in [2, cols + 2) where floor(rel_col * ratio) changed from the previous pixel
    rc = np.arange(1, cols + 2, dtype=np.int64)
    dc = (rc * cr) >> F; prev = ((rc - 1) * cr) >> F
    oc = rc[(rc >= 2) & (dc != prev)]
    dv = ((((oc * cr) >> F) * crec) & (ONE - 1))
    idx = [np.clip(oc - 3 + k, 0, cols - 1) for k in range(4)]
    out = []
    for r in range(2, rows + 2):
        if drow(r) == drow(r - 1):
            continue
        du = (drow(r) * rrec) & (ONE - 1)
        w = [P[min(max(r - 3 + k, 0), sh - 1)] for k in range(4)]
        yb = cubic(w[0], w[1], w[2], w[3], du)                # vertical pass, every column
        v = cubic(yb[idx[0]], yb[idx[1]], yb[idx[2]], yb[idx[3]], dv)
        q = np.where(v < 0, -((-v) >> F), v >> F)            # ap_fixed -> ap_uint<8>: toward zero, then wrap
        out.append((q & 0xFF).astype(np.uint8))
    flat = np.concatenate(out)
    return flat[:dw * dh], flat.size


def test_image(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    img = 128 + 60 * np.sin(x / 37.0) * np.cos(y / 53.0) + 40 * (x / w) - 30 * (y / h)
    img += np.random.default_rng(20261016).normal(0, 8, (h, w))
    return np.clip(img, 0, 255).astype(np.uint8)


p = argparse.ArgumentParser()
p.add_argument('xclbin'); p.add_argument('--srcw', type=int, default=7680); p.add_argument('--srch', type=int, default=4320)
p.add_argument('--sizes', default='512x512,1920x1080,3840x2160'); p.add_argument('--input', help='raw 8-bit image, srcw x srch')
a = p.parse_args()
sw, sh = a.srcw, a.srch
assert sw % 8 == 0 and sw <= 8192 and sh <= 8192
img = np.fromfile(a.input, np.uint8, sw * sh).reshape(sh, sw) if a.input else test_image(sw, sh)
c = Card(a.xclbin); k = c.kernel('kernel_resize')
cfg = c.bo(k, 0, 64); src = c.bo(k, 1, sw * sh).put(img); ok, times = True, []
for s in a.sizes.split(','):
    dw, dh = map(int, s.split('x'))
    assert dw <= sw and dh <= sh, 'the kernel only scales down'
    cfg.put(np.array([sw, sh, dw, dh], np.uint32))
    dst = c.bo(k, 2, (dw * dh + 7) // 8 * 8).zero()
    t = timed_timeout(c, k, cfg, src, dst)
    got = dst.get(np.uint8, dw * dh)
    want, emitted = model(img, dw, dh)
    diff = got.astype(np.int16) - want
    nbad = int(np.count_nonzero(diff)); ok = ok and nbad == 0 and emitted >= dw * dh; times.append(t)
    print('resize %dx%d -> %dx%d: kernel %.2f ms; %d of %d pixels differ from the model (max |diff| %d)%s'
          % (sw, sh, dw, dh, t * 1e3, nbad, dw * dh, int(np.abs(diff).max()) if nbad else 0,
             '' if emitted >= dw * dh else ', model emits only %d pixels' % emitted))
sys.exit(result('resize', ok, ', '.join('%.1f ms' % (t * 1e3) for t in times)))
