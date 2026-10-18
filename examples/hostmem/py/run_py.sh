#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] -- the no-compile alternative to ../run.sh: the same probe (kernel writes / reads host RAM through
# peer_window windows 2/3; bandwidth; dependent-read latency) with the Python host hostmem_host.py instead of the C++
# hostmem_host. The kernel module innova2_hostmem.ko is still needed: it is built here with make (kernel headers) unless
# it is already loaded. Unloads the module at exit if it loaded it.
P=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$P/../../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/hostmem.xclbin}"); H=$REPO/fld_lite/hostmem; source /opt/xilinx/xrt/setup.sh >/dev/null
if ! lsmod | grep -q "^innova2_hostmem"; then
  make -s -C $H >/dev/null || { echo "*** build innova2_hostmem.ko (kernel module: needs the kernel headers)"; exit 1; }
  sudo -n insmod $H/innova2_hostmem.ko bdf=$USER_BDF || exit 1
  trap 'sudo -n rmmod innova2_hostmem' EXIT
fi
sudo -n env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-} \
  python3 "$P/hostmem_host.py" $USER_BDF $X
