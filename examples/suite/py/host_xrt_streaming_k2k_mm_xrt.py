#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/streaming_k2k_mm_xrt -- Python (pyxrt) port of Vitis_Accel_Examples
host_xrt/streaming_k2k_mm_xrt/src/host.cpp (commit d1a35c39): krnl_stream_vadd (memory in, stream out) ->
krnl_stream_vmult (stream + memory in, memory out); 4K ints, a = b = c = i, the result must be (a + b) * c. Only the
memory and size arguments are set (the stream arguments are connected in the xclbin), both runs started, then waited."""
import sys
import numpy as np
import pyxrt
from suite_lib import Card, parse, result, xclbin, i32

a = parse(__doc__)
c = Card(xclbin(a.path, 'krnl_stream_vadd_vmult.xclbin'))
ka, km = c.kernel('krnl_stream_vadd'), c.kernel('krnl_stream_vmult')
N = 4 * 1024
x = np.arange(N, dtype=np.int32)
b0 = c.bo(ka, 0, N * 4).put(x); b1 = c.bo(ka, 1, N * 4).put(x); b2 = c.bo(km, 0, N * 4).put(x)
out = c.bo(km, 2, N * 4).zero()
r = pyxrt.run(ka); r.set_arg(0, b0.bo); r.set_arg(1, b1.bo); r.set_arg(3, N); r.start()
r1 = pyxrt.run(km); r1.set_arg(0, b2.bo); r1.set_arg(2, out.bo); r1.set_arg(3, N); r1.start()
r.wait(); r1.wait()
want = i32((x.astype(np.int64) + x) * x)
sys.exit(result('host_xrt/streaming_k2k_mm_xrt', np.array_equal(out.get(np.int32), want)))
