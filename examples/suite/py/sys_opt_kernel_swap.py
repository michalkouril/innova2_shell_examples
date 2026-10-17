#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""sys_opt/kernel_swap -- Python (pyxrt) port of Vitis_Accel_Examples sys_opt/kernel_swap/src/host.cpp (commit
d1a35c39, an OpenCL host): two xclbins loaded one after the other on the same card. krnl_vmul.xclbin: c = a * b
(1024 ints, a = b = i); its context, kernel and buffers are then released and krnl_vadd.xclbin is loaded:
krnl_vadd(c, c) must give 2 * a * b.
  usage: sys_opt_kernel_swap.py <build_dir> (or -x1 <krnl_vmul.xclbin> -x2 <krnl_vadd.xclbin>)"""
import argparse
import gc
import sys
import numpy as np
from suite_lib import Card, result, xclbin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('path', nargs='?', default='.'); p.add_argument('-x1'); p.add_argument('-x2')
a = p.parse_args()
x1 = a.x1 or xclbin(a.path, 'krnl_vmul.xclbin'); x2 = a.x2 or xclbin(a.path, 'krnl_vadd.xclbin')
N = 1024
x = np.arange(N, dtype=np.int32)
c = Card(x1); k = c.kernel('krnl_vmul')
da = c.bo(k, 0, N * 4).put(x); db = c.bo(k, 1, N * 4).put(x); dm = c.bo(k, 2, N * 4).zero()
c.call(k, da, db, dm, N)
h_temp = dm.get(np.int32)
ok = np.array_equal(h_temp, x * x)
detail = 'vmul ok' if ok else 'vmul mismatch'
del k, da, db, dm, c
gc.collect()                                # release the first xclbin's context before loading the second
if ok:
    c = Card(x2); k = c.kernel('krnl_vadd')
    dt = c.bo(k, 0, N * 4).put(h_temp); dt2 = c.bo(k, 1, N * 4).put(h_temp); dadd = c.bo(k, 2, N * 4).zero()
    c.call(k, dt, dt2, dadd, N)
    ok = np.array_equal(dadd.get(np.int32), 2 * x * x)
    detail += ', vadd after the swap ' + ('ok' if ok else 'mismatch')
sys.exit(result('sys_opt/kernel_swap', ok, detail))
