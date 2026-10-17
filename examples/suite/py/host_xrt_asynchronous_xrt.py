#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/asynchronous_xrt -- Python (pyxrt) port of Vitis_Accel_Examples host_xrt/asynchronous_xrt/src/host.cpp
(commit d1a35c39). AMD's host queues the two input syncs on two xrt::queue objects, makes the kernel run wait for
both, then queues the output sync. pyxrt has no xrt::queue, so the same ordering is built from Python futures: the two
input syncs run concurrently in a thread pool, the run starts when both are done, the output sync follows it.
vadd of 1M ints (a[i] = b[i] = i)."""
import sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from suite_lib import Card, parse, result, xclbin
from pyxrt_util import TO_DEV, FROM_DEV

a = parse(__doc__)
c = Card(xclbin(a.path, 'vadd.xclbin'))
k = c.kernel('vadd')
N = 1024 * 1024
x = np.arange(N, dtype=np.int32)
b0 = c.bo(k, 0, N * 4); b1 = c.bo(k, 1, N * 4); out = c.bo(k, 2, N * 4)
b0.mem.view(np.int32)[:] = x; b1.mem.view(np.int32)[:] = x; out.mem[:] = 0
with ThreadPoolExecutor(2) as main_queue:
    e0 = main_queue.submit(b0.bo.sync, TO_DEV, N * 4, 0)        # main_queue.enqueue(bo0.sync)
    e1 = main_queue.submit(b1.bo.sync, TO_DEV, N * 4, 0)        # queue_bo1.enqueue(bo1.sync)
    def run_after_inputs():                                      # main_queue.enqueue(bo1_event); run.start(); wait()
        e0.result(); e1.result()
        return c.call(k, b0, b1, out, N)
    er = main_queue.submit(run_after_inputs)
    er.result()
    eo = main_queue.submit(out.bo.sync, FROM_DEV, N * 4, 0)     # bo_out_event
    eo.result()
sys.exit(result('host_xrt/asynchronous_xrt', np.array_equal(out.mem.view(np.int32), x + x)))
