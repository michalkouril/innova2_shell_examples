#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.
# SPDX-License-Identifier: Apache-2.0
"""host_xrt/mailbox_auto_restart_xrt -- Python (pyxrt) port of Vitis_Accel_Examples
host_xrt/mailbox_auto_restart_xrt/src/host.cpp (commit d1a35c39): mbox_autorestart (add = in1 + in2, mult = in1 * in2)
is started once in auto-restart mode (pyxrt.autostart(0): until stopped); 20 iterations alternate between writing new
inputs through the mailbox (pyxrt.mailbox set_arg + write) and reading the outputs back (read + get_arg), which must
equal the host's sum and product; then the run is stopped."""
import sys
import pyxrt
from suite_lib import Card, parse, result, xclbin

a = parse(__doc__)
c = Card(xclbin(a.path, 'mbox_autorestart.xclbin'))
k = c.kernel('mbox_autorestart')
adder1 = adder2 = 20
run = pyxrt.run(k)
run.set_arg(0, adder1); run.set_arg(1, adder2)
run.start(pyxrt.autostart(0))
mbox = pyxrt.mailbox(run)
ok, detail = True, ''
for cnt in range(20):
    if cnt % 2 == 0:
        adder1 += 1; adder2 -= 1
        mbox.set_arg(0, adder1); mbox.set_arg(1, adder2); mbox.write()
    else:
        mbox.read()
        hw_add = int.from_bytes(mbox.get_arg(2)[:4], 'little', signed=True)
        hw_mult = int.from_bytes(mbox.get_arg(3)[:4], 'little', signed=True)
        if hw_add != adder1 + adder2 or hw_mult != adder1 * adder2:
            ok = False
            detail = 'iteration %d: add %d (want %d), mult %d (want %d)' % (cnt, hw_add, adder1 + adder2, hw_mult,
                                                                         adder1 * adder2)
            break
run.stop()
run.wait(1000)
sys.exit(result('host_xrt/mailbox_auto_restart_xrt', ok, detail))
