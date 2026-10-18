#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] -- lat_probe with a Python host: how long one memory access takes from a kernel, nothing compiled.
#
# What it shows: the same flow as ../run.sh (DDR mode 'on', then 'hybrid', the probe in each, 'on' restored at exit)
#   with the Python host lat_host.py instead of fld_lite/lat/lat_host.
# Needs: the card running inn2x r10; python3, numpy, XRT's pyxrt; passwordless sudo. No compiler, no cable.
# Run:   examples/lat_probe/py/run_py.sh [xclbin]      (default <repo>/xclbin/lat_probe.xclbin); USER_BDF picks the card
# Prints and PASS: as ../run.sh -- per mode the kernel clock and four "N ns/op = N cycles" lines; no verdict line.
# Takes: about 10 s.

# 1. the card, the xclbin, XRT
P=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$P/../../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/lat_probe.xclbin}"); source /opt/xilinx/xrt/setup.sh >/dev/null
# 2. whatever happens, DDR mode back to 'on' at exit
trap 'sudo -n innova2_ddr_mode --bdf $MGMT_BDF on | grep "in force"' EXIT
# 3. each DDR mode: set it, then lat_host.py as root with this shell's XRT environment (7 = repetitions)
for m in on hybrid; do
  echo "== DDR mode $m"; sudo -n innova2_ddr_mode --bdf $MGMT_BDF $m | grep 'in force'
  sudo -n env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-} \
    python3 "$P/lat_host.py" $USER_BDF $X 7
done
