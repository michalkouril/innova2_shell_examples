#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""rtl_kernels/rtl_adder_streams -- Python (pyxrt) port of Vitis_Accel_Examples rtl_kernels/rtl_adder_streams/src/host.cpp
(commit d1a35c39, an OpenCL host): three RTL kernels connected by AXI streams -- input stage (memory -> stream),
adder stage (+ 10), output stage (stream -> memory); 4096 ints (i) must come out as i + 10. All three are started,
then waited for."""
import sys
import numpy as np
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'adder.xclbin'))
ki, ka, ko = (c.kernel(n) for n in ('krnl_input_stage_rtl', 'krnl_adder_stage_rtl', 'krnl_output_stage_rtl'))
N, INC = 4096, 10
x = np.arange(N, dtype=np.int32)
bi = c.bo(ki, 0, N * 4).put(x); bo = c.bo(ko, 0, N * 4).zero()
runs = [c.start(ki, bi, N), c.start(ka, INC, N), c.start(ko, bo, N)]
for r in runs:
    r.wait()
sys.exit(result('rtl_kernels/rtl_adder_streams', np.array_equal(bo.get(np.int32), x + INC)))
