#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/streaming_free_running_k2k_xrt -- Python (pyxrt) port of Vitis_Accel_Examples
host_xrt/streaming_free_running_k2k_xrt/src/host.cpp (commit d1a35c39): mem_read -> increment (free-running, never
started by the host) -> mem_write over AXI streams; 1M ints (i) in, each must come out + 1. mem_write is started
before mem_read, both waited for."""
import sys
import numpy as np
import pyxrt
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'krnl_incr.xclbin'))
kr, kw = c.kernel('mem_read'), c.kernel('mem_write')
N = 1024 * 1024
x = np.arange(N, dtype=np.int32)
bi = c.bo(kr, 0, N * 4).put(x); bo = c.bo(kw, 0, N * 4).zero()
wr = pyxrt.run(kw); wr.set_arg(0, bo.bo); wr.set_arg(1, N); wr.start()
rd = pyxrt.run(kr); rd.set_arg(0, bi.bo); rd.set_arg(1, N); rd.start()
rd.wait(); wr.wait()
sys.exit(result('host_xrt/streaming_free_running_k2k_xrt', np.array_equal(bo.get(np.int32), x + 1)))
