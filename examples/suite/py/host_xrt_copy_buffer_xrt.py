#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/copy_buffer_xrt -- Python (pyxrt) port of Vitis_Accel_Examples host_xrt/copy_buffer_xrt/src/host.cpp
(commit d1a35c39): a[i] = 13, b = copy of a, vector_add(out, a, b, 1024), out == a + a. AMD's host copies a to b
device-side with xrt::bo::copy; pyxrt (XRT 2.27) has no bo.copy, so the port copies through the host mapping
(a is read back from the device first, so the copy still carries what the device holds)."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'vector_addition.xclbin'))
k = c.kernel('vector_add')
N = 1024
ba = c.bo(k, 0, N * 4).put(np.full(N, 13, dtype=np.int32)); bb = c.bo(k, 1, N * 4).zero(); out = c.bo(k, 2, N * 4).zero()
bb.put(ba.get(np.int32))                      # bo_b.copy(bo_a)
c.call(k, out, ba, bb, N)
sys.exit(result('host_xrt/copy_buffer_xrt', np.array_equal(out.get(np.int32), np.full(N, 26, dtype=np.int32)),
                'copy through the host (pyxrt has no bo.copy)'))
