#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""sys_opt/multiple_process -- Python (pyxrt) port of Vitis_Accel_Examples sys_opt/multiple_process/src/host.cpp
(commit d1a35c39, an OpenCL host): three processes share one xclbin (krnl_vadd, krnl_vsub, krnl_vmul) and the card at
the same time; each opens the device itself, runs its kernel on 1024 random ints and checks the result. As AMD's host
forks the children before any of them opens the device, the port starts them with multiprocessing ('spawn', so no
XRT state is inherited); XCL_MULTIPROCESS_MODE=1 is set as there (current XRT does not need it)."""
import multiprocessing as mp
import os
import sys
import numpy as np


def child(path, kid):
    from suite_lib import Card, i32, rand31
    name = ('krnl_vadd', 'krnl_vsub', 'krnl_vmul')[kid]
    sa, sb = rand31(1024, seed=2 * kid + 1).astype(np.int64), rand31(1024, seed=2 * kid + 2).astype(np.int64)
    want = i32((sa + sb, sa - sb, sa * sb)[kid])
    c = Card(path); k = c.kernel(name)
    ba = c.bo(k, 0, 4096).put(i32(sa)); bb = c.bo(k, 1, 4096).put(i32(sb)); bc = c.bo(k, 2, 4096).zero()
    c.call(k, ba, bb, bc, 1024)
    ok = np.array_equal(bc.get(np.int32), want)
    print('[PID: %d] %s: %s' % (os.getpid(), name, 'match' if ok else 'MISMATCH'), flush=True)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    from suite_lib import parse, result, xclbin
    a = parse(__doc__)
    path = xclbin(a.path, 'multi_krnl.xclbin')
    os.environ['XCL_MULTIPROCESS_MODE'] = '1'
    ctx = mp.get_context('spawn')
    ps = [ctx.Process(target=child, args=(path, i)) for i in range(3)]
    for q in ps:
        q.start()
    for q in ps:
        q.join(300)
    codes = [q.exitcode for q in ps]
    sys.exit(result('sys_opt/multiple_process', codes == [0, 0, 0], 'child exit codes %s' % codes))
