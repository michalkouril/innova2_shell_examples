#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- an iperf2 UDP server in the FPGA, measured by a stock `iperf -c -u` on the host.
#
# What it shows: e5udp_host makes the FPGA an iperf2 UDP server on ConnectX-5 port 0 (192.168.77.2:5001, jumbo
#   frames); from the network namespace on port 1 a stock `iperf -c ... -u` sends to it. iperf_srv in the FPGA counts
#   the datagrams, losses and reordering and answers the final datagram with iperf2's server report, so the client
#   prints the FPGA's view ("Server Report"). The tests run back to back on one xclbin load (iperf_srv starts a new
#   test at the first data datagram after a final one).
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo; iperf (iperf2), ethtool, XRT's pyxrt
#   (iperf_sctl.py reads the FPGA's counters). MTU 9000; port 1 in network namespace innova2c; HYBRID DDR mode during
#   the run, 'on' after.
# Run:   examples/iperf/run.sh [xclbin]      (default <repo>/xclbin/iperf.xclbin)
#   IPERF_SECS   seconds per test (default 5)
#   IPERF_LENS   datagram payload sizes of the single-stream tests (default "64 1470 8192")
#   IPERF_PLENS  the same for the multi-stream tests (default "64 1470 8192"); IPERF_PAR streams at once (default 4)
# Prints: per single-stream test the client's own line, the Server Report (the FPGA's numbers), the FPGA's counters and
#   the NIC's hardware receive counter on port 0 (rx_packets_phy); per multi-stream test datagrams/s from the FPGA's
#   counters; three 1 s tests back to back; "fld_tx frames N, iperf_srv reports N: ACCOUNTED"; "DMA failed ...".
# Reading it: one iperf2 client socket sends at most ~0.6-0.8 M datagrams/s, so small datagrams show the client's
#   rate, not the FPGA's; the multi-stream tests show what the FPGA receives. PASS: ACCOUNTED and DMA failed 0, with
#   the FPGA's and the NIC's counts in agreement.
# Takes: about 1 min with IPERF_SECS=5. Logs: run.XXXXXX/ next to this script.

# 1. the network library (and the card), the xclbin, a run directory, the test length; the kernel clock from the
#    xclbin (default 300 MHz): iperf_sctl.py needs it to turn cycles into durations
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/iperf.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX"); T=${IPERF_SECS:-5}
MHZ=$(xclbinutil -i $X --info 2>/dev/null | awk '/Name:.*DATA_CLK/{f=1} f && /Frequency:/{print int($2); exit}'); MHZ=${MHZ:-300}
# 2. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, both ports at MTU 9000
preflight; netns_up 9000
# 3. the endpoint: UDP port 5001, BlueFlame, jumbo frames; --secs is only an upper bound (60 + 12 x IPERF_SECS): the
#    script ends it through the stop file
endpoint $L/host.log $X $IB0 $CX5_BDF --port 5001 --secs $(( 60 + 12 * T )) --bf --jumbo --stop-file $L/stop || exit 1
# 4. NSX runs a command in the namespace; PYX runs iperf_sctl.py (pyxrt) with this shell's XRT environment: first it
#    sets iperf_srv's port and clock, later (--counters) it prints the FPGA's counters
NSX="sudo -n ip netns exec $NS"
PYX="sudo -n env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-} python3 $D/iperf_sctl.py $X --bdf $USER_BDF"
$PYX --port 5001 --mhz $MHZ; echo "kernel clock $MHZ MHz (the report's durations and jitter)"
# nic: the NIC's hardware receive counter on port 0 -- every frame that reached the port, whatever the FPGA did
nic() { ethtool -S $NET0 2>/dev/null | awk '/ rx_packets_phy:/{print $2}'; }
# 5. single-stream tests: -b 10G asks for more than one socket can send, so the client sends as fast as it can
for len in ${IPERF_LENS:-64 1470 8192}; do
  echo "== iperf -c $IPF -u -l $len -b 10G -t $T"
  n0=$(nic)
  $NSX iperf -c $IPF -u -l $len -b 10G -t $T -i $T 2>&1 | grep -E "Server Report|^\[ *[0-9]+\] .*sec" | tail -3
  n1=$(nic)
  $PYX --mhz $MHZ --counters
  echo "NIC port 0 rx_packets_phy: +$(( n1 - n0 )) frames"
done
# 6. multi-stream tests:
# the FPGA's own receive rate: IPERF_PAR client streams at once (iperf -P; one socket is the limit above). iperf_srv
# keeps one test at a time, so their merged Server Report is not meaningful here: the FPGA's and the NIC's counters are
for len in ${IPERF_PLENS:-64 1470 8192}; do
  echo "== $len B, ${IPERF_PAR:-4} client streams at once (rate from the FPGA's / NIC's counters)"
  n0=$(nic); d0=$($PYX --mhz $MHZ --counters | grep -o 'data [0-9]*' | grep -o '[0-9]*')
  $NSX iperf -c $IPF -u -l $len -b 10G -t $T -P ${IPERF_PAR:-4} > /dev/null 2>&1
  n1=$(nic); d1=$($PYX --mhz $MHZ --counters | grep -o 'data [0-9]*' | grep -o '[0-9]*')
  echo "FPGA data datagrams +$(( d1 - d0 )), NIC port 0 rx_packets_phy +$(( n1 - n0 )): $(( (d1 - d0) / T )) datagrams/s over $T s, $(( (d1 - d0) / T * len * 8 / 1000000 )) Mb/s payload"
done
# 7. three short tests back to back without a reload (iperf_srv starts a new test each time); then the number
echo "== 3 short tests back to back (no reload)"
for i in 1 2 3; do $NSX iperf -c $IPF -u -l 1470 -b 1G -t 1 2>&1 | grep -E "^\[ *[0-9]+\] +[0-9.]+-[0-9.]+ sec .* pps" | tail -1; done
#    of server reports iperf_srv has sent, from its counters (shown, too)
rep=$($PYX --mhz $MHZ --counters | tee /dev/stderr | grep -o 'reports [0-9]*' | grep -o '[0-9]*$') 2>&1
# 8. stop the endpoint and account for every frame it sent:
# e5udp_host's verdict line says PROBLEM when fld_tx sent fewer frames than fld_rx received: here fld_tx sends only
# the server reports, so the check is fld_tx frames == iperf_srv's reports
touch $L/stop; endpoint_wait $L/host.log
tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host.log | grep -o '[0-9]*$')
echo "fld_tx frames ${tx:-?}, iperf_srv reports ${rep:-?}: $([ -n "$tx" ] && [ "$tx" = "$rep" ] && echo ACCOUNTED || echo "NOT ACCOUNTED")"
