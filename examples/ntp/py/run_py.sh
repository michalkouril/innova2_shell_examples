#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] -- the no-compile alternative to ../run.sh, same flow and checks: the FPGA NTP server on ConnectX
# port 0 (192.168.77.2:123) with FLD-lite set up by fld_lite/py/e5udp_host.py; ntp_servo.py, ntp_client.py and chronyd
# -Q as in run.sh; the load generator is ntp_load.py (Python worker processes, one datagram per system call, so its
# replies/s are the client's limit, well below the C ntp_load's). NTP_LOCAL_STRATUM as in run.sh.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
X=$(readlink -f "${1:-$REPO/xclbin/ntp.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
# 1. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, MTU 1500
preflight; netns_up 1500
# 2. the FPGA's NTP endpoint: UDP port 123, BlueFlame doorbells; it stops after 300 s or as soon as $L/stop exists
endpoint $L/host.log $X $IB0 $CX5_BDF --port 123 --secs 300 --bf --stop-file $L/stop || exit 1
# 3. the servo, in the background. $NSX runs a command in the namespace with this shell's XRT environment (the servo
#    and the client use pyxrt). The servo must lock within 60 s, else the run ends here.
NSX="sudo -n ip netns exec $NS env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-}"
$NSX python3 "$D/ntp_servo.py" --xclbin $X --bdf $USER_BDF --server $IPF --local-stratum ${NTP_LOCAL_STRATUM:-10} \
     --status $L/servo.json --pause-file $L/pause --stop-file $L/stop > $L/servo.log 2>&1 < /dev/null &
SV=$!
for w in $(seq 60); do grep -q '"locked": true' $L/servo.json 2>/dev/null && break; sleep 1; done
grep -E "COARSE|FREQ" $L/servo.log; tail -2 $L/servo.log
grep -q '"locked": true' $L/servo.json 2>/dev/null || { echo "*** the servo did not lock"; touch $L/stop; wait $SV; endpoint_wait $L/host.log; exit 1; }
sleep 10                                                       # let the PI loop settle past the lock threshold
# 4. the client's checks, a stock client, the load
echo "== client checks"
$NSX python3 "$D/ntp_client.py" --xclbin $X --bdf $USER_BDF --server $IPF --status $L/servo.json --pause-file $L/pause
echo "== chronyd -Q (a stock client, 8 samples)"
$NSX chronyd -Q -t 20 "server $IPF iburst minpoll -6 maxpoll -6 maxsamples 8" "pidfile /run/chrony/innova2_ntpq.pid" "cmdport 0" 2>&1 | tail -2
echo "== load (threads x window in flight)"
for tw in 1:1 1:32 2:32 4:32; do
  $NSX python3 "$P/ntp_load.py" --server $IPF --threads ${tw%:*} --window ${tw#*:} --secs 5 --quiet
done
# 5. the servo's last intervals; stop the servo and print its final counters
echo "== servo, last 3 intervals"; grep SERVO $L/servo.log | tail -3
touch $L/stop; wait $SV; grep COUNTERS $L/servo.log
# 6. stop the endpoint and account for every frame
# e5udp_host.py's verdict line says PROBLEM when fld_tx sent fewer frames than fld_rx received: here that difference must be
# exactly the frames ntp_resp dropped on purpose (ntp_client.py's six drop tests)
endpoint_wait $L/host.log
rx=$(grep -o 'fld_rx  (done): CQEs [0-9]*, frames [0-9]*' $L/host.log | grep -o '[0-9]*$'); tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host.log | grep -o '[0-9]*$')
dr=$(grep COUNTERS $L/servo.log | tail -1 | sed -E 's/.*drop_len=([0-9]+) drop_hdr=([0-9]+) drop_mode=([0-9]+).*/\1+\2+\3/')
echo "frames in $rx, replies sent $tx, dropped by ntp_resp $((dr)): $([ -n "$rx" ] && [ $((rx - tx)) = $((dr)) ] && echo "ACCOUNTED" || echo "NOT ACCOUNTED")"
