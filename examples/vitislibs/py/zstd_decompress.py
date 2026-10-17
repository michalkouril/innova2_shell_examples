#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# zstd_decompress.py <xclbin> [--input FILE | --mb N] [--levels L,...] [--zst FILE] -- pyxrt host for AMD Vitis
# Libraries data_compression/L2/demos/zstd_decompress (host.cpp + common/libs/compress/zstdOCLHost.cpp,
# decompressEngine): the stock zstd CLI compresses the input (window 32 KB, --zstd=wlog=15: the kernel is built with a
# 32 KB block/window), then xilGzipS2MM(out, size, status, out capacity) and xilGzipMM2S(in, size, last = 1) are
# started and the time of xilZstdDecompressStream is measured (as AMD's host), output compared byte for byte.
# One stream per xclbin load: the kernel (stock and fixed, with AMD's C++ host as well) decodes the first stream after a
# load and hangs on the next one (2026-10-17, RESULTS_part1.md). So one level runs per invocation; --levels with several
# needs --reset-xclbin <any other xclbin>: each level then runs in a child process after that xclbin was loaded.
# Stock kernel: level 3 by default -- it also hangs on zstd -19 streams (amd_ip_bugs V17); zstd_decompress_fix.py
# defaults to level 19. Default input: 32 MB of the host's copyright texts (README: 952 MB/s of output at level 3).
import argparse, os, subprocess, sys, time
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', '..', 'common')); sys.path.insert(0, here)
from pyxrt_util import Card, result, start_args, wait_ok
import vl_stream as vs


def decompress(c, zst, out_cap, timeout_s=60):
    mm2s, s2mm, dec = c.kernel('xilGzipMM2S'), c.kernel('xilGzipS2MM'), c.kernel('xilZstdDecompressStream')
    inb = c.bo(mm2s, 0, len(zst)).put(np.frombuffer(zst, np.uint8))
    outb = c.bo(s2mm, 0, out_cap).zero(); szb = c.bo(s2mm, 1, 64).zero(); stb = c.bo(s2mm, 2, 64).zero()
    rr = start_args(s2mm, {0: outb, 1: szb, 2: stb, 3: out_cap})
    rw = start_args(mm2s, {0: inb, 1: len(zst), 2: 1})
    t0 = time.perf_counter()
    rd = start_args(dec, {})
    ok = wait_ok(rd, timeout_s)
    kt = time.perf_counter() - t0
    ok = ok and wait_ok(rr, timeout_s) and wait_ok(rw, timeout_s)
    if not ok:
        raise RuntimeError('zstd decompress ' + vs.STUCK % timeout_s)
    n = int(szb.get(np.uint32, 1)[0])
    return outb.get(np.uint8, min(n, out_cap)).tobytes(), kt


def main(name, levels):
    p = argparse.ArgumentParser()
    p.add_argument('xclbin'); p.add_argument('--input'); p.add_argument('--mb', type=float, default=32)
    p.add_argument('--levels', default=levels, help='zstd levels to test, comma separated')
    p.add_argument('--zst', help='decompress this .zst instead (its original: --input)')
    p.add_argument('--reset-xclbin', help='another xclbin, loaded between streams (needed for several --levels)')
    a = p.parse_args()
    levels = a.levels.split(',')
    if len(levels) > 1 and not a.zst:
        if not a.reset_xclbin:
            p.error('the kernel decodes one stream per xclbin load: give one level, or --reset-xclbin <other xclbin>')
        ok, rates = True, []
        for l in levels:
            subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, %r); from pyxrt_util import Card; '
                            'Card(%r)' % (os.path.join(here, '..', '..', '..', 'common'), a.reset_xclbin)], check=True)
            r = subprocess.run([sys.executable, os.path.abspath(sys.argv[0]), a.xclbin, '--levels', l, '--mb', str(a.mb)]
                               + (['--input', a.input] if a.input else []), capture_output=True, text=True, timeout=300)
            sys.stdout.write(''.join(x for x in r.stdout.splitlines(True) if not x.startswith('PYHOST-')))
            ok = ok and r.returncode == 0
            rates += [x.split(': ', 1)[1].strip() for x in r.stdout.splitlines() if x.startswith('PYHOST-') and ': ' in x]
        return result(name, ok, '; '.join(rates))
    data = vs.load_input(a.input, int(a.mb * (1 << 20)))
    c = Card(a.xclbin); ok, rates = True, []
    cases = [('file', open(a.zst, 'rb').read())] if a.zst else \
        [('level %s' % l, subprocess.run(['zstd', '-q', '-c', '-%s' % l, '--zstd=wlog=15', '--stream-size=%d' % len(data)]
                                         + (['--ultra'] if int(l) > 19 else []),
                                         input=data, capture_output=True, check=True).stdout) for l in levels]
    for label, zst in cases:
        out, kt = decompress(c, zst, len(data) + (1 << 20))
        same = out == data
        ok = ok and same; rates.append('%s %.0f MB/s' % (label, len(data) / kt / 1e6))
        print('%s: %s, %d B -> %d B, kernel %.2f ms, %.2f MB/s (output), %s'
              % (name, label, len(zst), len(out), kt * 1e3, len(data) / kt / 1e6, 'identical' if same else 'DIFFERENT'))
    return result(name, ok, '; '.join(rates))


if __name__ == '__main__':
    sys.exit(main('zstd_decompress', '3'))
