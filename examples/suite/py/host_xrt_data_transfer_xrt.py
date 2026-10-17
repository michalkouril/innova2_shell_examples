#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/data_transfer_xrt -- Python (pyxrt) port of Vitis_Accel_Examples host_xrt/data_transfer_xrt/src/host.cpp
(commit d1a35c39): the buffer write / read API (xrt::bo::write, read) instead of mapping: 256 ints (i) written to the
input, dummy_kernel copies them, the output read back must equal the input."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin
from pyxrt_util import TO_DEV, FROM_DEV

a = parse(__doc__)
c = Card(xclbin(a.path, 'dummy_kernel.xclbin'))
k = c.kernel('dummy_kernel')
N = 256
x = np.arange(N, dtype=np.int32)
bi = c.bo(k, 0, N * 4); bo = c.bo(k, 1, N * 4)
bi.bo.write(x, 0); bi.bo.sync(TO_DEV, N * 4, 0)
c.call(k, bi, bo, N)
bo.bo.sync(FROM_DEV, N * 4, 0)
y = np.asarray(bo.bo.read(N * 4, 0)).view(np.int32)
sys.exit(result('host_xrt/data_transfer_xrt', np.array_equal(y, x)))
