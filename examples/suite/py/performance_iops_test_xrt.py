#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""performance/iops_test_xrt -- Python (pyxrt) port of Vitis_Accel_Examples performance/iops_test_xrt/src/host.cpp
(commit d1a35c39): 10000 runs of the trivial 'hello' kernel are prepared (one 20-byte buffer each); for each command
count from 10 to 1,000,000 the host keeps up to 10000 of them in flight (start; wait the oldest; restart it) and prints
commands per second. The loop is Python, so the rate is lower than the C++ host's and says as much about the host
language as about XRT/the card. Extra check (not in AMD's host): one buffer must hold the kernel's "Hello World\\n".
  usage: performance_iops_test_xrt.py <xclbin | build_dir> [--max N] (largest command count, default 1000000)"""
import sys
import time
import pyxrt
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__, lambda p: p.add_argument('--max', type=int, default=1000000))
c = Card(xclbin(a.path, 'hello.xclbin'))
k = c.kernel('hello')
counts = [n for n in (10, 50, 100, 200, 500, 1000, 1500, 2000, 3000, 5000, 10000, 50000, 100000, 500000, 1000000)
          if n <= a.max]
EXP = 10000
bos = [c.bo(k, 0, 20).zero() for _ in range(EXP)]
cmds = []
for b in bos:
    r = pyxrt.run(k); r.set_arg(0, b.bo); cmds.append(r)
print('Allocated commands, expect %d, created %d' % (EXP, len(cmds)))
best = 0
for num in counts:
    issued = completed = i = 0
    t0 = time.perf_counter()
    for cmd in cmds:
        cmd.start(); issued += 1
        if issued == num:
            break
    while completed < num:
        cmds[i].wait(); completed += 1
        if issued < num:
            cmds[i].start(); issued += 1
        i += 1
        if i == len(cmds):
            i = 0
    dt = time.perf_counter() - t0
    best = max(best, num / dt)
    print('Commands: %7d iops: %.0f' % (num, num / dt), flush=True)
hello = bytes(bos[0].get(count=13))
sys.exit(result('performance/iops_test_xrt', hello == b'Hello World\n\0', 'peak %.0f commands/s (Python loop)' % best))
