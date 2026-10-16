#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host: builds innova2_hostmem.ko and hostmem_host (fld_lite/hostmem), loads the
# module, runs the probe (read / write bandwidth and dependent-read latency to host RAM), unloads the module.
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/hostmem.xclbin}"); H=$REPO/fld_lite/hostmem; source /opt/xilinx/xrt/setup.sh >/dev/null
make -s -C $H >/dev/null || { echo "*** build innova2_hostmem.ko"; exit 1; }
g++ -std=c++17 -O2 -o $H/hostmem_host $H/hostmem_host.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib -lxrt_coreutil -pthread -Wl,-rpath,$XILINX_XRT/lib || exit 1
lsmod | grep -q "^innova2_hostmem" || sudo -n insmod $H/innova2_hostmem.ko bdf=$USER_BDF || exit 1
trap 'sudo -n rmmod innova2_hostmem' EXIT
sudo -n env PATH=$PATH $H/hostmem_host $USER_BDF $X
