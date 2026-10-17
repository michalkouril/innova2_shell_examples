#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""rtl_kernels/rtl_streaming_k2k_mm -- Python (pyxrt) port of Vitis_Accel_Examples rtl_kernels/rtl_streaming_k2k_mm/src/host.cpp
(commit d1a35c39, an OpenCL host): two host-started RTL adders (myadder1, myadder2: started with no arguments) between krnl_mm2s and krnl_s2mm,
over AXI streams; 1024 * 1024 random ints, each must come out + 2. krnl_mm2s and
krnl_s2mm get only their memory and size arguments (argument 1 is the stream)."""
import sys
import numpy as np
import pyxrt
from suite_lib import Card, parse, result, xclbin, rand31, i32

a = parse(__doc__)
c = Card(xclbin(a.path, 'myadder.xclbin'))
k_mm2s, k_s2mm = c.kernel('krnl_mm2s'), c.kernel('krnl_s2mm')
N = 1024 * 1024
x = rand31(N)
bi = c.bo(k_mm2s, 0, N * 4).put(x); bo = c.bo(k_s2mm, 0, N * 4).zero()
runs = []
r = pyxrt.run(k_mm2s); r.set_arg(0, bi.bo); r.set_arg(2, N); r.start(); runs.append(r)
for name in ('myadder1:{myadder1_1}', 'myadder2:{myadder2_1}'):
    r = pyxrt.run(c.kernel(name)); r.start(); runs.append(r)
r = pyxrt.run(k_s2mm); r.set_arg(0, bo.bo); r.set_arg(2, N); r.start(); runs.append(r)
for r in runs:
    r.wait()
sys.exit(result('rtl_kernels/rtl_streaming_k2k_mm', np.array_equal(bo.get(np.int32), i32(x.astype(np.int64) + 2))))
