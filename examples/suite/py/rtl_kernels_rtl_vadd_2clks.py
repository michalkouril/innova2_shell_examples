#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""rtl_kernels/rtl_vadd_2clks -- Python (pyxrt) port of Vitis_Accel_Examples rtl_kernels/rtl_vadd_2clks/src/host.cpp (commit
d1a35c39, an OpenCL host): an RTL vadd kernel with two clocks (control and data path on different clocks); 256 ints, a = b = i, the result must be a + b."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'vadd.xclbin'))
k = c.kernel('krnl_vadd_2clk_rtl')
N = 256
x = np.arange(N, dtype=np.int32)
b1 = c.bo(k, 0, N * 4).put(x); b2 = c.bo(k, 1, N * 4).put(x); w = c.bo(k, 2, N * 4).zero()
c.call(k, b1, b2, w, N)
sys.exit(result('rtl_kernels/rtl_vadd_2clks', np.array_equal(w.get(np.int32), x + x)))
