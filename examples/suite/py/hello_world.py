#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""hello_world -- Python (pyxrt) port of Vitis_Accel_Examples hello_world/src/host.cpp (commit d1a35c39): vadd of
4096 ints (a[i] = b[i] = i), result compared with a + b."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'vadd.xclbin'))
k = c.kernel('vadd')
N = 4096
x = np.arange(N, dtype=np.int32)
b0 = c.bo(k, 0, N * 4).put(x); b1 = c.bo(k, 1, N * 4).put(x); out = c.bo(k, 2, N * 4).zero()
c.call(k, b0, b1, out, N)
sys.exit(result('hello_world', np.array_equal(out.get(np.int32), x + x)))
