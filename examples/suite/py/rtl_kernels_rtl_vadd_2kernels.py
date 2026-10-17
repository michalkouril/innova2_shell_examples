#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""rtl_kernels/rtl_vadd_2kernels -- Python (pyxrt) port of Vitis_Accel_Examples rtl_kernels/rtl_vadd_2kernels/src/host.cpp (commit
d1a35c39, an OpenCL host): two RTL vadd kernels in sequence, 256 ints (all inputs i): krnl_vadd_rtl_0(in1, in2) -> buffer 0, buffer 0 copied to
buffer 1, krnl_vadd_rtl_1(buffer 1, in4) -> out, which must be in1 + in2 + in4. AMD's host copies device-side
(enqueueCopyBuffer); pyxrt (XRT 2.27) has no bo.copy, so the port copies through the host after reading buffer 0 back."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'vadd.xclbin'))
k0, k1 = c.kernel('krnl_vadd_rtl_0'), c.kernel('krnl_vadd_rtl_1')
N = 256
x = np.arange(N, dtype=np.int32)
r1 = c.bo(k0, 0, N * 4).put(x); r2 = c.bo(k0, 1, N * 4).put(x); rw0 = c.bo(k0, 2, N * 4).zero()
rw1 = c.bo(k1, 0, N * 4).put(x); r3 = c.bo(k1, 1, N * 4).put(x); w = c.bo(k1, 2, N * 4).zero()
c.call(k0, r1, r2, rw0, N)
rw1.put(rw0.get(np.int32))                 # enqueueCopyBuffer(rw_0, rw_1)
c.call(k1, rw1, r3, w, N)
sys.exit(result('rtl_kernels/rtl_vadd_2kernels', np.array_equal(w.get(np.int32), 3 * x), 'copy through the host (pyxrt has no bo.copy)'))
