#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] -- the no-compile alternative to ../run.sh, same flow and checks: the FPGA serves memcached on
# ConnectX port 0 (192.168.77.2:11211, jumbo frames) with FLD-lite set up by fld_lite/py/e5udp_host.py; mcd_client.py
# runs the SET/GET/DELETE checks from the network namespace on port 1; then the load generator mcd_load.py (Python,
# worker processes) measures throughput and latency. Python sends one datagram per system call, so the load points
# show the client's rate, well below the C mcd_load's -- the FPGA's own rate is the same.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
X=$(readlink -f "${1:-$REPO/xclbin/memcached.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
preflight; netns_up 9000
endpoint $L/host.log $X $IB0 $CX5_BDF --port 11211 --secs 200 --bf --jumbo --mcd --dump 10 || exit 1
sudo -n ip netns exec $NS python3 "$D/mcd_client.py" $IPF 11211 64
# --inorder: the Xilinx pipeline does not return the request's opaque field, but answers in order; --check-every 16
# byte-checks every 16th GET hit against the preloaded value
ld() { sudo -n ip netns exec $NS python3 "$P/mcd_load.py" --server $IPF --port 11211 --keys ${MCD_KEYS:-65536} --secs 5 --quiet --inorder \
         --check-every 16 "$@" | grep -E '^RESULT|^preload|^\*\*\*'; }
echo "== load: preload (1 in flight), then GETs and GET/SET mixes, 32 B values (threads x window in flight)"
ld --threads 1 --window 1 --vsize 32 --secs 2
for tw in 1:8 2:32 4:16 4:64 4:16:10 4:16:50 4:64:100; do
  IFS=: read t w sp <<< "$tw"; ld --threads $t --window $w --set-pct ${sp:-0} --vsize 32 --no-preload
done
endpoint_wait $L/host.log 300
grep -E "memcached debug" $L/host.log | tail -3
