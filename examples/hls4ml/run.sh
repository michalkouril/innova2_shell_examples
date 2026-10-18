#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- a neural network in the FPGA answering over UDP: hls4ml's jet-tagging model.
#
# What it shows: e5udp_host makes the FPGA a UDP endpoint on ConnectX-5 port 0 (192.168.77.2:5005) whose kernel runs the
#   hls4ml model. From a network namespace on port 1, h4ml_client.py sends all 166,000 test jets (jets.bin), checks
#   every reply bit for bit against hls4ml's emulation, then measures ping-pong latency.
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo. MTU 1500; port 1 in network namespace
#   innova2c; HYBRID DDR mode during the run, 'on' after.
# Run:   examples/hls4ml/run.sh [xclbin]      (default <repo>/xclbin/hls4ml.xclbin)
# Prints: the endpoint's "UP:" line; "jets 166000 replies 166000 lost 0 mismatching 0 ..." and the ping-pong round trip;
#   H4ML-CLIENT PASS; the endpoint's counters and verdict; "DMA failed during this run: N".
# PASS: H4ML-CLIENT PASS, E5UDP-HOST OK, DMA failed 0.
# Takes: about 2.5 min (the endpoint always runs its full 150 s).

# 1. the network library (and the card), the xclbin, a run directory for the endpoint's log
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/hls4ml.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
# 2. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, MTU 1500
preflight; netns_up 1500
# 3. the endpoint: UDP port 5005 steered to the FPGA, 150 s, BlueFlame doorbells
endpoint $L/host.log $X $IB0 $CX5_BDF --port 5005 --secs 150 --bf || exit 1
# 4. the client, from the namespace
sudo -n ip netns exec $NS python3 "$D/h4ml_client.py" $IPF 5005 "$D/jets.bin"
# 5. wait for the endpoint's 150 s to run out; its counters and verdict
endpoint_wait $L/host.log
