#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- memcached in the FPGA: the FPGA answers memcached's binary protocol over UDP by itself.
#
# What it shows: e5udp_host makes the FPGA a memcached server on ConnectX-5 port 0 (192.168.77.2:11211, jumbo frames).
#   From a network namespace on port 1, mcd_client.py checks SET / GET / DELETE on 64 keys with 1 - 2000 B values and
#   the error cases (its GET rate is one request at a time: 1 / round trip); then mcd_load, a multi-threaded C load
#   generator with a window of requests in flight per thread, measures throughput and latency percentiles,
#   byte-checking every 16th GET hit.
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo, gcc. Sets MTU 9000 on both ports and
#   moves port 1 into network namespace innova2c; HYBRID DDR mode during the run, 'on' after.
# Run:   examples/memcached/run.sh [xclbin]      (default <repo>/xclbin/memcached.xclbin)
#   MCD_KEYS  keys for mcd_load (default 65536). The FPGA's hash table has 131072 buckets of 4 keys (17 hash bits;
#             64 MB) and its value store 524288 slots of 2 KB (1 GB); a SET whose bucket is full is refused -- about
#             0.03 % of 65536 keys (C simulation: 18 of k00000000..k00065535), counted as "failed" / "err".
# Prints: the endpoint's "UP:" line; mcd_client's 10 PASS / FAIL lines and "MCD-CLIENT PASS (0 failed of 10)"; a
#   "preload:" line, then one "RESULT threads= window= ... set_pct=" line per load (req/s, lost, bad, latency p50 ..
#   p99.9 in us); the endpoint's FLD-lite counters and verdict; the last memcached debug counters; "DMA failed ...".
# PASS: MCD-CLIENT PASS; every RESULT with lost 0, bad 0, unmatched 0 (err = refused SETs, see MCD_KEYS);
#   E5UDP-HOST OK; DMA failed 0.
# Takes: about 3.5 min (the endpoint always runs its full 200 s). Logs: run.XXXXXX/ next to this script.

# 1. the network library (and the card), the xclbin; a run directory for the logs and the load generator
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/memcached.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
# 2. build mcd_load
gcc -O2 -pthread -o "$L/mcd_load" "$D/mcd_load.c" || exit 1
# 3. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, both ports at MTU 9000
preflight; netns_up 9000
# 4. the FPGA's memcached endpoint on port 0. e5udp_host options:
#      --port 11211  the UDP port steered to the FPGA      --secs 200  run 200 s, then stop and print the counters
#      --bf          BlueFlame doorbells (see fld_echo/run.sh)
#      --jumbo       frames up to 9000 B (16 KB slots)
#      --mcd         the memcached xclbin: udp_rx_lite -> mcd -> udp_tx_lite, 64 MB hash table + 1 GB value store
#      --dump 10     print the memcached debug counters every 10 s
endpoint $L/host.log $X $IB0 $CX5_BDF --port 11211 --secs 200 --bf --jumbo --mcd --dump 10 || exit 1
# 5. the functional checks, from the namespace: 64 keys
sudo -n ip netns exec $NS python3 "$D/mcd_client.py" $IPF 11211 64
# 6. the load runs. ld <options>: one mcd_load run (5 s) on $MCD_KEYS keys; the grep keeps its summary lines.
# --inorder: the Xilinx pipeline does not return the request's opaque field, but answers in order; --check-every 16
# byte-checks every 16th GET hit against the preloaded value
ld() { sudo -n ip netns exec $NS "$L/mcd_load" --server $IPF --port 11211 --keys ${MCD_KEYS:-65536} --secs 5 --quiet --inorder \
         --check-every 16 "$@" | grep -E '^RESULT|^preload|^\*\*\*'; }
# Until 2026-10-17 the pipeline stalled under load (GETs at 3 x 21 in flight, SETs that overwrite a key); the three
# causes are fixed (mcd_kernel.cpp v4, mcd_hls.tcl, memWrite.cpp) and kernels/mcd_load_tb.cpp checks them in C simulation.
# With 4 client threads the host's 4 cores, not the FPGA, set the rate.
echo "== load: preload (1 in flight), then GETs and GET/SET mixes, 32 B values (threads x window in flight)"
#    first the preload (SETs every key, 1 in flight), then 2 s of GETs with 1 in flight
ld --threads 1 --window 1 --vsize 32 --secs 2
#    then threads:window[:SET percent]: GETs only, then 10 %, 50 % and 100 % SETs; the keys are already loaded
for tw in 1:8 2:32 4:16 4:64 4:16:10 4:16:50 4:64:100; do
  IFS=: read t w sp <<< "$tw"; ld --threads $t --window $w --set-pct ${sp:-0} --vsize 32 --no-preload
done
# 7. wait for the endpoint's 200 s to run out (at most 300 s), print its counters and the last debug counters
endpoint_wait $L/host.log 300
grep -E "memcached debug" $L/host.log | tail -3
