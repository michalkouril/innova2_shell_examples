#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host, after fld_lite/host_setup.sh: per-operation latency of a kernel's memory port
# (bank0 offset 0 = the on-chip window in HYBRID DDR mode, offset 2 MB = DDR), in ns and kernel cycles. Restores DDR
# mode 'on' at exit.
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/lat_probe.xclbin}"); source /opt/xilinx/xrt/setup.sh >/dev/null
trap 'sudo -n innova2_ddr_mode --bdf $MGMT_BDF on | grep "in force"' EXIT
for m in on hybrid; do echo "== DDR mode $m"; sudo -n innova2_ddr_mode --bdf $MGMT_BDF $m | grep 'in force'
  sudo -n env PATH=$PATH "$REPO/fld_lite/lat/lat_host" $USER_BDF $X 7; done
