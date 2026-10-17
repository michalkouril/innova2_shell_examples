#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""performance/axi_burst_performance -- Python (pyxrt) port of Vitis_Accel_Examples
performance/axi_burst_performance/src/host.cpp (commit d1a35c39, an OpenCL host): for the 256-bit and the 512-bit
xclbin, each of the 6 kernels (burst length x outstanding transactions) writes, then reads, a 16 MB buffer (filled
with 0xFF first); the kernel counts its own cycles and its read errors into a 4 x int64 info buffer; throughput =
size / (cycles / frequency) (a WRITE run reports 0 cycles
on this card, with AMD's C++ host as well, which then prints 'inf GB/sec'; so does the port). As in AMD's host the frequency is an option, default 300 MHz (the clock the kernels were
built for; the card's actual data clock may differ). Unlike AMD's host, which keeps only the last kernel's error
count, every kernel's count must be 0.
  usage: performance_axi_burst_performance.py <build_dir> [-f MHz] [-m MB]  (or -x1 <256-bit xclbin> -x2 <512-bit>)"""
import argparse
import gc
import sys
import numpy as np
from suite_lib import Card, result, xclbin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('path', nargs='?', default='.')
p.add_argument('-x1'); p.add_argument('-x2')
p.add_argument('-f', type=float, default=300.0, help='kernel frequency in MHz (default 300)')
p.add_argument('-m', type=int, default=16, help='buffer size in MB (default 16)')
a = p.parse_args()
files = [a.x1 or xclbin(a.path, 'test_kernel_maxi_256bit.xclbin'), a.x2 or xclbin(a.path, 'test_kernel_maxi_512bit.xclbin')]
size = a.m * 1024 * 1024
errors, lines = 0, []
for width, f in zip(('256', '512'), files):
    c = Card(f)
    ks = [c.kernel('test_kernel_maxi_%sbit_%d' % (width, i + 1)) for i in range(6)]
    info = c.bo(ks[0], 2, 32).zero()
    data = c.bo(ks[0], 3, size).put(np.full(size, 255, dtype=np.uint8))
    for direction, dname in ((0, 'WRITE'), (1, 'READ')):
        print('Kernel->AXI Burst %s performance (%s-bit)' % (dname, width))
        for k in ks:
            c.call(k, size, direction, info, data)
            cyc, err, bl, ot = (int(v) for v in info.get(np.int64))
            # a WRITE run reports 0 cycles on this card with AMD's host too (it then prints inf GB/sec)
            gbps = size / (cyc * 1000.0 / a.f / 1e9) / (1024 ** 3) if cyc else float('inf')
            errors += err
            print('Data Width = %s burst_length = %d num_outstanding = %d buffer_size = %d MB | throughput = %.4f GB/sec'
                  ' (%d cycles, errors %d)' % (width, bl, ot, a.m, gbps, cyc, err))
    del ks, k, info, data, c                  # release this xclbin before loading the next
    gc.collect()
sys.exit(result('performance/axi_burst_performance', errors == 0, 'read errors %d (at -f %g MHz)' % (errors, a.f)))
