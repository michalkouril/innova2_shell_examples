#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host: e5udp_host makes the FPGA an NTP server on ConnectX port 0
# (192.168.77.2:123); in the network namespace on port 1: ntp_servo.py disciplines the FPGA clock to this host's clock
# and publishes the reply fields, ntp_client.py checks the replies and the drops (verdict NTP-CLIENT PASS),
# chronyd -Q measures the server as a stock client would, and ntp_load measures replies/s, loss and latency.
# NTP_LOCAL_STRATUM (default 10): the stratum to serve when the host's own clock is not synchronised (refid LOCL).
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/ntp.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
gcc -O2 -pthread -o "$L/ntp_load" "$D/ntp_load.c" || exit 1
preflight; netns_up 1500
endpoint $L/host.log $X $IB0 $CX5_BDF --port 123 --secs 300 --bf --stop-file $L/stop || exit 1
NSX="sudo -n ip netns exec $NS env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-}"
$NSX python3 "$D/ntp_servo.py" --xclbin $X --bdf $USER_BDF --server $IPF --local-stratum ${NTP_LOCAL_STRATUM:-10} \
     --status $L/servo.json --pause-file $L/pause --stop-file $L/stop > $L/servo.log 2>&1 < /dev/null &
SV=$!
for w in $(seq 60); do grep -q '"locked": true' $L/servo.json 2>/dev/null && break; sleep 1; done
grep -E "COARSE|FREQ" $L/servo.log; tail -2 $L/servo.log
grep -q '"locked": true' $L/servo.json 2>/dev/null || { echo "*** the servo did not lock"; touch $L/stop; wait $SV; endpoint_wait $L/host.log; exit 1; }
sleep 10                                                       # let the PI loop settle past the lock threshold
echo "== client checks"
$NSX python3 "$D/ntp_client.py" --xclbin $X --bdf $USER_BDF --server $IPF --status $L/servo.json --pause-file $L/pause
echo "== chronyd -Q (a stock client, 8 samples)"
$NSX chronyd -Q -t 20 "server $IPF iburst minpoll -6 maxpoll -6 maxsamples 8" "pidfile /run/chrony/innova2_ntpq.pid" "cmdport 0" 2>&1 | tail -2
echo "== load (threads x window in flight)"
for tw in 1:1 1:32 2:32 4:32; do
  $NSX "$L/ntp_load" --server $IPF --threads ${tw%:*} --window ${tw#*:} --secs 5 --quiet
done
echo "== servo, last 3 intervals"; grep SERVO $L/servo.log | tail -3
touch $L/stop; wait $SV; grep COUNTERS $L/servo.log
# e5udp_host's verdict line says PROBLEM when fld_tx sent fewer frames than fld_rx received: here that difference must be
# exactly the frames ntp_resp dropped on purpose (ntp_client.py's six drop tests)
endpoint_wait $L/host.log
rx=$(grep -o 'fld_rx  (done): CQEs [0-9]*, frames [0-9]*' $L/host.log | grep -o '[0-9]*$'); tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host.log | grep -o '[0-9]*$')
dr=$(grep COUNTERS $L/servo.log | tail -1 | sed -E 's/.*drop_len=([0-9]+) drop_hdr=([0-9]+) drop_mode=([0-9]+).*/\1+\2+\3/')
echo "frames in $rx, replies sent $tx, dropped by ntp_resp $((dr)): $([ -n "$rx" ] && [ $((rx - tx)) = $((dr)) ] && echo "ACCOUNTED" || echo "NOT ACCOUNTED")"
