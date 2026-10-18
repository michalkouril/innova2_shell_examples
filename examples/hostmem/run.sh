#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- a kernel reading and writing host RAM directly.
#
# What it shows: innova2_hostmem.ko provides two 1 MB DMA buffers in host RAM; hostmem_host points the shell's
#   peer_window windows 2 and 3 at them, and the kernel writes them, reads them back, and measures bandwidth and
#   dependent-read latency to host RAM.
# Needs: the card running inn2x r10; XRT, g++, make, kernel headers (it builds innova2_hostmem.ko and hostmem_host in
#   fld_lite/hostmem); passwordless sudo. No cable, no DDR mode change.
# Run:   examples/hostmem/run.sh [xclbin]      (default <repo>/xclbin/hostmem.xclbin); USER_BDF picks the card
# Prints: a "card:" line; "1. kernel WRITE -> host RAM ... PASS", "2. kernel READ <- host RAM ... PASS",
#   "3. bandwidth: ... GB/s", "4. latency: dependent read from host RAM N ns", the peer_window counters.
# PASS: lines 1 and 2 say PASS. The module is unloaded again at exit.
# Takes: under a minute, most of it the build.

# 1. the card, the xclbin, XRT
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/hostmem.xclbin}"); H=$REPO/fld_lite/hostmem; source /opt/xilinx/xrt/setup.sh >/dev/null
# 2. build the module and the host program
make -s -C $H >/dev/null || { echo "*** build innova2_hostmem.ko"; exit 1; }
g++ -std=c++17 -O2 -o $H/hostmem_host $H/hostmem_host.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib -lxrt_coreutil -pthread -Wl,-rpath,$XILINX_XRT/lib || exit 1
# 3. load the module for this card (unloaded again at exit), run the probe
lsmod | grep -q "^innova2_hostmem" || sudo -n insmod $H/innova2_hostmem.ko bdf=$USER_BDF || exit 1
trap 'sudo -n rmmod innova2_hostmem' EXIT
sudo -n env PATH=$PATH $H/hostmem_host $USER_BDF $X
