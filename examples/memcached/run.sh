#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host: the FPGA serves memcached (binary protocol over UDP) on ConnectX port 0
# (192.168.77.2:11211, jumbo frames); mcd_client.py in a network namespace on port 1 runs SET/GET/DELETE checks with
# 64 B - 1 KB values (its GET rate is one request at a time: 1 / round trip); then mcd_load, a multi-threaded C load
# generator with a window of requests in flight per thread, measures throughput and latency percentiles, byte-checking
# every 16th GET hit. MCD_KEYS (default 256): the demo hash table has 128 buckets (7 hash bits) and refuses a SET
# whose bucket is full: into an empty table 7 of mcd_load's 256 keys (k00000000..k00000255) do not fit, exactly as in
# C simulation; here mcd_client's 64 keys are already in it, so the preload refuses more (237 ok / 19 on 2026-10-17)
# and those keys stay misses / refused SETs in the load runs.
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/memcached.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
gcc -O2 -pthread -o "$L/mcd_load" "$D/mcd_load.c" || exit 1
preflight; netns_up 9000
endpoint $L/host.log $X $IB0 $CX5_BDF --port 11211 --secs 200 --bf --jumbo --mcd --dump 10 || exit 1
sudo -n ip netns exec $NS python3 "$D/mcd_client.py" $IPF 11211 64
# --inorder: the Xilinx pipeline does not return the request's opaque field, but answers in order; --check-every 16
# byte-checks every 16th GET hit against the preloaded value
ld() { sudo -n ip netns exec $NS "$L/mcd_load" --server $IPF --port 11211 --keys ${MCD_KEYS:-256} --secs 5 --quiet --inorder \
         --check-every 16 "$@" | grep -E '^RESULT|^preload|^\*\*\*'; }
# Until 2026-10-17 the pipeline stalled under load (GETs at 3 x 21 in flight, SETs that overwrite a key); the three
# causes are fixed (mcd_kernel.cpp v4, mcd_hls.tcl, memWrite.cpp) and kernels/mcd_load_tb.cpp checks them in C simulation.
# With 4 client threads the host's 4 cores, not the FPGA, set the rate.
echo "== load: preload (1 in flight), then GETs and GET/SET mixes, 32 B values (threads x window in flight)"
ld --threads 1 --window 1 --vsize 32 --secs 2
for tw in 1:8 2:32 4:16 4:64 4:16:10 4:16:50 4:64:100; do
  IFS=: read t w sp <<< "$tw"; ld --threads $t --window $w --set-pct ${sp:-0} --vsize 32 --no-preload
done
endpoint_wait $L/host.log 300
grep -E "memcached debug" $L/host.log | tail -3
