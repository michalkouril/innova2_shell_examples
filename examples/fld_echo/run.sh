#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [frames] -- on the card's host: FLD-lite as a line-rate echo with no CPU in the data path. The FPGA's
# generator (gen_tx) and FLD-lite sit on the two ConnectX ports, joined by the DAC loop; FLD-lite (fld_rx -> afu_loop ->
# fld_tx) echoes every frame back and sink_rx byte-checks 1 frame in 16. Sweep 64 B - 1500 B at gap 0, then the idle
# echo latency (e5_host, receive CQE to transmit CQE).
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/fld_echo.xclbin}"); NF=${2:-200000}
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 1500 up; done; sleep 5
for len in 64 256 512 1024 1500; do
  printf 'len %-5s: ' $len
  sudo -n env PATH=$PATH timeout 150 $E5/e5gen_host $USER_BDF $X $IB0 $IB1 $CX5_BDF $NF --len $len --gap 0 --bf --inline 222 --inline-adapt --rxk 4 --sample 4 2>&1 \
    | grep -E '^gen_tx|sink_rx|frames/s|Gb/s|\*\*\*' | tr '\n' ' '; echo
done
echo '== idle echo latency (64 B)'
sudo -n env PATH=$PATH timeout 150 $E5/e5_host $USER_BDF $X $IB0 $IB1 $CX5_BDF 2000 --lat --len 64 --bf --inline 222 --pre 1026 2>&1 | grep -E 'lat|median|p99|\*\*\*' | tail -6
