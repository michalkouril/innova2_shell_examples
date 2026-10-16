#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host: e5udp_host makes the FPGA a UDP endpoint on ConnectX port 0 (192.168.77.2:5005);
# h4ml_client.py in a network namespace on port 1 sends all 166,000 test jets, checks every reply bit for bit against
# hls4ml's emulation, then measures ping-pong latency. Verdict: H4ML-CLIENT PASS.
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/hls4ml.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
preflight; netns_up 1500
endpoint $L/host.log $X $IB0 $CX5_BDF --port 5005 --secs 150 --bf || exit 1
sudo -n ip netns exec $NS python3 "$D/h4ml_client.py" $IPF 5005 "$D/jets.bin"
endpoint_wait $L/host.log
