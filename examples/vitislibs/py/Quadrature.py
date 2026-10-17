#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Quadrature.py <quad_hcf_kernel.xclbin> [--tol T] [--extra N] [--seed S] -- Python (pyxrt) host for Vitis Libraries
quantitative_finance/L2/demos/Quadrature (ported from src/host/quad_hcf_test.cpp and test_data.hpp):
quad_hcf_kernel(in, out, num) prices European calls under the Heston model by adaptive quadrature of its
characteristic function.

Default: AMD's one test vector (S 80, K 100, T 0.5, v0 0.01, r 0.05, rho -0.5, sigma 0.1, theta 0.5, kappa 1;
expected 2.308462), integration tolerance --tol (default 1e-4), pass when |price - expected| <= 0.001 (TEST_TOLERANCE).
--extra N adds N seeded random contracts, priced on the host with a numpy Heston reference (Gil-Pelaez integrals of the
"little trap" characteristic function; it gives 2.3084595 for AMD's vector) and checked with the same 0.001.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, result  # noqa: E402

TEST_TOLERANCE = 0.001
# struct hcfEngineInputDataType (quad_hcf_engine_def.hpp): float s, k, v, t, r, rho, vvol, vbar, kappa, tol; int pad[6]
REC = np.dtype([('s', '<f4'), ('k', '<f4'), ('v', '<f4'), ('t', '<f4'), ('r', '<f4'), ('rho', '<f4'), ('vvol', '<f4'),
                ('vbar', '<f4'), ('kappa', '<f4'), ('tol', '<f4'), ('pad', '<i4', (6,))])
AMD = dict(s=80, k=100, t=0.5, v=0.01, r=0.05, rho=-0.5, vvol=0.1, vbar=0.5, kappa=1, exp=2.308462)
_trap = getattr(np, 'trapezoid', None) or np.trapz


def heston_call(s, k, t, v0, r, rho, sig, th, ka):
    u = np.linspace(1e-6, 400, 400001)

    def cf(w):
        a = ka - rho * sig * 1j * w
        d = np.sqrt(a * a + sig * sig * (1j * w + w * w))
        g = (a - d) / (a + d)
        e = np.exp(-d * t)
        C = (ka * th / sig ** 2) * ((a - d) * t - 2 * np.log((1 - g * e) / (1 - g)))
        D = (v0 / sig ** 2) * (a - d) * (1 - e) / (1 - g * e)
        return np.exp(1j * w * (np.log(s) + r * t) + C + D)

    lk = np.log(k)
    p2 = 0.5 + _trap(np.real(np.exp(-1j * u * lk) * cf(u) / (1j * u)), u) / np.pi
    p1 = 0.5 + _trap(np.real(np.exp(-1j * u * lk) * cf(u - 1j) / (1j * u * s * np.exp(r * t))), u) / np.pi
    return s * p1 - k * np.exp(-r * t) * p2


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('xclbin')
    ap.add_argument('--tol', type=float, default=0.0001, help='integration tolerance (AMD default 1e-4)')
    ap.add_argument('--extra', type=int, default=0, help='random contracts checked against the numpy reference')
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    tests = [AMD]
    r = np.random.default_rng(a.seed)
    for _ in range(a.extra):
        p = dict(s=r.uniform(80, 120), k=r.uniform(80, 120), t=r.uniform(0.25, 2), v=r.uniform(0.01, 0.09),
                 r=r.uniform(0, 0.06), rho=r.uniform(-0.8, 0), vvol=r.uniform(0.1, 0.5), vbar=r.uniform(0.01, 0.09),
                 kappa=r.uniform(0.5, 3))
        f = {key: float(np.float32(val)) for key, val in p.items()}   # the kernel sees float32 inputs
        p['exp'] = heston_call(f['s'], f['k'], f['t'], f['v'], f['r'], f['rho'], f['vvol'], f['vbar'], f['kappa'])
        tests.append(p)
    n = len(tests)
    rec = np.zeros(n, dtype=REC)
    for i, p in enumerate(tests):
        for key in ('s', 'k', 'v', 't', 'r', 'rho', 'vvol', 'vbar', 'kappa'):
            rec[i][key] = p[key]
        rec[i]['tol'] = a.tol
    c = Card(a.xclbin)
    k = c.kernel('quad_hcf_kernel')
    bin_ = c.bo(k, 0, rec.nbytes).put(rec)
    bout = c.bo(k, 1, max(n, 16) * 4).zero()
    t = c.timed(k, bin_, bout, n)
    out = bout.get(np.float32, n)
    exp = np.array([p['exp'] for p in tests])
    diff = np.abs(out - exp)
    fails = int(np.sum(diff > TEST_TOLERANCE))
    for i in np.nonzero(diff > TEST_TOLERANCE)[0][:10]:
        print('    FPGA[%d]: expected %.6f, got %.6f, diff %.6f' % (i, exp[i], out[i], diff[i]))
    print('INFO: %d test(s), integration tolerance %g: out of tolerance %d, max error %.3g; run kernel %.0f us'
          % (n, a.tol, fails, diff.max(), 1e6 * t))
    return result('Quadrature', fails == 0, '%d tests, %d out of tolerance, max err %.3g' % (n, fails, diff.max()))


if __name__ == '__main__':
    sys.exit(main())
