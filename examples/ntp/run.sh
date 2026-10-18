#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- an NTP server in the FPGA: the FPGA timestamps and answers NTP requests by itself.
#
# What it shows: e5udp_host makes the FPGA an NTP server on ConnectX-5 port 0 (192.168.77.2:123). In the network
#   namespace on port 1: ntp_servo.py disciplines the FPGA's clock to this host's clock and publishes the reply fields
#   (stratum, refid, ...); ntp_client.py checks the replies (offset, delay, the FPGA's residence time) and that
#   malformed requests are dropped; chronyd -Q measures the server as a stock client would; ntp_load measures
#   replies/s, loss and latency.
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo; gcc, chrony, XRT's pyxrt (the servo and
#   the client read and steer the FPGA's clock through XRT). MTU 1500; port 1 in network namespace innova2c; HYBRID
#   DDR mode during the run, 'on' after.
# Run:   examples/ntp/run.sh [xclbin]      (default <repo>/xclbin/ntp.xclbin)
#   NTP_LOCAL_STRATUM  the stratum served when the host's own clock is not synchronised (refid LOCL; default 10)
# Prints: the servo's COARSE / FREQ lines and its latest SERVO lines; the client's summary and "NTP-CLIENT PASS";
#   chronyd's result; one RESULT line per load; the servo's last 3 SERVO lines and COUNTERS; the endpoint's counters;
#   "frames in N, replies sent N, dropped by ntp_resp N: ACCOUNTED"; "DMA failed during this run: N".
# PASS: the servo LOCKED; NTP-CLIENT PASS; RESULT lines with lost 0 and bad 0; ACCOUNTED; DMA failed 0. The endpoint's
#   own verdict reads "E5UDP-HOST PROBLEM" here, as expected: the client's six drop tests get no reply, so fld_tx sends
#   6 frames fewer than fld_rx received -- the ACCOUNTED line checks exactly that.
# Takes: about 1 min. Logs: run.XXXXXX/ next to this script.

# 1. the network library (and the card), the xclbin; a run directory (logs, the servo's status / pause / stop files)
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/ntp.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
# 2. build ntp_load
gcc -O2 -pthread -o "$L/ntp_load" "$D/ntp_load.c" || exit 1
# 3. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, MTU 1500
preflight; netns_up 1500
# 4. the FPGA's NTP endpoint: UDP port 123, BlueFlame doorbells; it stops after 300 s or as soon as $L/stop exists
endpoint $L/host.log $X $IB0 $CX5_BDF --port 123 --secs 300 --bf --stop-file $L/stop || exit 1
# 5. the servo, in the background. $NSX runs a command in the namespace with this shell's XRT environment (the servo
#    and the client use pyxrt). The servo must lock within 60 s, else the run ends here.
NSX="sudo -n ip netns exec $NS env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-}"
$NSX python3 "$D/ntp_servo.py" --xclbin $X --bdf $USER_BDF --server $IPF --local-stratum ${NTP_LOCAL_STRATUM:-10} \
     --status $L/servo.json --pause-file $L/pause --stop-file $L/stop > $L/servo.log 2>&1 < /dev/null &
SV=$!
for w in $(seq 60); do grep -q '"locked": true' $L/servo.json 2>/dev/null && break; sleep 1; done
grep -E "COARSE|FREQ" $L/servo.log; tail -2 $L/servo.log
grep -q '"locked": true' $L/servo.json 2>/dev/null || { echo "*** the servo did not lock"; touch $L/stop; wait $SV; endpoint_wait $L/host.log; exit 1; }
sleep 10                                                       # let the PI loop settle past the lock threshold
# 6. the client's checks, a stock client, the load
echo "== client checks"
$NSX python3 "$D/ntp_client.py" --xclbin $X --bdf $USER_BDF --server $IPF --status $L/servo.json --pause-file $L/pause
echo "== chronyd -Q (a stock client, 8 samples)"
$NSX chronyd -Q -t 20 "server $IPF iburst minpoll -6 maxpoll -6 maxsamples 8" "pidfile /run/chrony/innova2_ntpq.pid" "cmdport 0" 2>&1 | tail -2
echo "== load (threads x window in flight)"
for tw in 1:1 1:32 2:32 4:32; do
  $NSX "$L/ntp_load" --server $IPF --threads ${tw%:*} --window ${tw#*:} --secs 5 --quiet
done
# 7. the servo's last intervals; stop the servo and print its final counters
echo "== servo, last 3 intervals"; grep SERVO $L/servo.log | tail -3
touch $L/stop; wait $SV; grep COUNTERS $L/servo.log
# 8. stop the endpoint and account for every frame
# e5udp_host's verdict line says PROBLEM when fld_tx sent fewer frames than fld_rx received: here that difference must be
# exactly the frames ntp_resp dropped on purpose (ntp_client.py's six drop tests)
endpoint_wait $L/host.log
rx=$(grep -o 'fld_rx  (done): CQEs [0-9]*, frames [0-9]*' $L/host.log | grep -o '[0-9]*$'); tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host.log | grep -o '[0-9]*$')
dr=$(grep COUNTERS $L/servo.log | tail -1 | sed -E 's/.*drop_len=([0-9]+) drop_hdr=([0-9]+) drop_mode=([0-9]+).*/\1+\2+\3/')
echo "frames in $rx, replies sent $tx, dropped by ntp_resp $((dr)): $([ -n "$rx" ] && [ $((rx - tx)) = $((dr)) ] && echo "ACCOUNTED" || echo "NOT ACCOUNTED")"
