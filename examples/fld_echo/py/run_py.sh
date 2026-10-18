#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] [frames] -- the no-compile alternative to ../run.sh, same sweep and verdicts: the FPGA's generator
# (gen_tx) and FLD-lite on the two ConnectX ports, FLD-lite echoing every frame, sink_rx byte-checking 1 in 16; set
# up by fld_lite/py/e5gen_host.py. The rates are the FPGA's (no CPU in the data path), as with the C host. Then the
# idle echo latency with fld_lite/py/e5_host.py, which sends from port 1 through the kernel (AF_PACKET) instead of a
# verbs QP: its round trip includes the Linux stack; the FPGA's own share (port 0 RX CQE -> TX CQE) is exact.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
X=$(readlink -f "${1:-$REPO/xclbin/fld_echo.xclbin}"); NF=${2:-200000}
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 1500 up; done; sleep 5
for len in 64 256 512 1024 1500; do
  printf 'len %-5s: ' $len
  sudo -n env $PYENV timeout 150 python3 $E5PY/e5gen_host.py $USER_BDF $X $IB0 $IB1 $CX5_BDF $NF --len $len --gap 0 --bf --inline 222 --inline-adapt --rxk 4 --sample 4 2>&1 \
    | grep -E '^gen_tx|sink_rx|frames/s|Gb/s|\*\*\*|Error' | tr '\n' ' '; echo
done
echo '== idle echo latency (64 B)'
sudo -n env $PYENV timeout 150 python3 $E5PY/e5_host.py $USER_BDF $X $IB0 $IB1 $CX5_BDF 2000 --lat --len 64 --bf --inline 222 --pre 1026 2>&1 | grep -E 'lat|median|p99|RESULT|\*\*\*|Error' | tail -6
