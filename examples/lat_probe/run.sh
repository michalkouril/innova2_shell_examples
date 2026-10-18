#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- how long one memory access takes from a kernel, in ns and kernel cycles.
#
# What it shows: lat_probe times single writes (with their acknowledgement) and single reads from a kernel's memory
#   port at bank0 offset 0 and offset 2 MB: first in DDR mode 'on' (both are DDR), then in HYBRID mode (offset 0 is
#   then the 1 MB on-chip URAM window, offset 2 MB still DDR).
# Needs: the card running inn2x r10; `bash fld_lite/host_setup.sh` run once (builds lat_host); passwordless sudo. No
#   cable. Switches the DDR mode (on, then hybrid) and restores 'on' at exit.
# Run:   examples/lat_probe/run.sh [xclbin]      (default <repo>/xclbin/lat_probe.xclbin); USER_BDF picks the card
# Prints: a "card:" line; per mode "== DDR mode <m>", the kernel clock and the loop overhead, then four lines
#   "write+ack | read   URAM window (off 0) | DDR (off 2 MB)   N ns/op = N cycles".
# PASS: there is no verdict line. Expect, in HYBRID mode, the URAM window faster than DDR, and in mode 'on' the two
#   equal (at 300 MHz: a read ~74 cycles from the window, ~106 from DDR).
# Takes: about 10 s.

# 1. the card, the xclbin, XRT
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/lat_probe.xclbin}"); source /opt/xilinx/xrt/setup.sh >/dev/null
# 2. whatever happens, DDR mode back to 'on' at exit
trap 'sudo -n innova2_ddr_mode --bdf $MGMT_BDF on | grep "in force"' EXIT
# 3. each DDR mode: set it, then lat_host (loads the xclbin; 7 = repetitions per measurement)
for m in on hybrid; do echo "== DDR mode $m"; sudo -n innova2_ddr_mode --bdf $MGMT_BDF $m | grep 'in force'
  sudo -n env PATH=$PATH "$REPO/fld_lite/lat/lat_host" $USER_BDF $X 7; done
