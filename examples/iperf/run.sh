#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] -- on the card's host: e5udp_host makes the FPGA an iperf2 UDP server on ConnectX port 0
# (192.168.77.2:5001, jumbo frames); in the network namespace on port 1 a stock `iperf -c ... -u` sends to it. For each
# test: the client's own line, the FPGA's numbers as iperf2 prints them ("Server Report"), the FPGA's counters
# (iperf_sctl.py) and the NIC's hardware receive counter on port 0 (rx_packets_phy) -- one iperf2 client socket sends
# at most ~0.6-0.8 M datagrams/s, so small datagrams show the client's rate, not the FPGA's. The tests run back to back
# on one xclbin load (iperf_srv starts a new test at the first data datagram after a final one).
# IPERF_SECS (default 5) per test; IPERF_LENS (default "64 1470 8192") datagram payload sizes.
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/iperf.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX"); T=${IPERF_SECS:-5}
MHZ=$(xclbinutil -i $X --info 2>/dev/null | awk '/Name:.*DATA_CLK/{f=1} f && /Frequency:/{print int($2); exit}'); MHZ=${MHZ:-300}
preflight; netns_up 9000
endpoint $L/host.log $X $IB0 $CX5_BDF --port 5001 --secs $(( 60 + 12 * T )) --bf --jumbo --stop-file $L/stop || exit 1
NSX="sudo -n ip netns exec $NS"
PYX="sudo -n env PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-} python3 $D/iperf_sctl.py $X --bdf $USER_BDF"
$PYX --port 5001 --mhz $MHZ; echo "kernel clock $MHZ MHz (the report's durations and jitter)"
nic() { ethtool -S $NET0 2>/dev/null | awk '/ rx_packets_phy:/{print $2}'; }
for len in ${IPERF_LENS:-64 1470 8192}; do
  echo "== iperf -c $IPF -u -l $len -b 10G -t $T"
  n0=$(nic)
  $NSX iperf -c $IPF -u -l $len -b 10G -t $T -i $T 2>&1 | grep -E "Server Report|^\[ *[0-9]+\] .*sec" | tail -3
  n1=$(nic)
  $PYX --mhz $MHZ --counters
  echo "NIC port 0 rx_packets_phy: +$(( n1 - n0 )) frames"
done
# the FPGA's own receive rate: IPERF_PAR client streams at once (iperf -P; one socket is the limit above). iperf_srv
# keeps one test at a time, so their merged Server Report is not meaningful here: the FPGA's and the NIC's counters are
for len in ${IPERF_PLENS:-64 1470 8192}; do
  echo "== $len B, ${IPERF_PAR:-4} client streams at once (rate from the FPGA's / NIC's counters)"
  n0=$(nic); d0=$($PYX --mhz $MHZ --counters | grep -o 'data [0-9]*' | grep -o '[0-9]*')
  $NSX iperf -c $IPF -u -l $len -b 10G -t $T -P ${IPERF_PAR:-4} > /dev/null 2>&1
  n1=$(nic); d1=$($PYX --mhz $MHZ --counters | grep -o 'data [0-9]*' | grep -o '[0-9]*')
  echo "FPGA data datagrams +$(( d1 - d0 )), NIC port 0 rx_packets_phy +$(( n1 - n0 )): $(( (d1 - d0) / T )) datagrams/s over $T s, $(( (d1 - d0) / T * len * 8 / 1000000 )) Mb/s payload"
done
echo "== 3 short tests back to back (no reload)"
for i in 1 2 3; do $NSX iperf -c $IPF -u -l 1470 -b 1G -t 1 2>&1 | grep -E "^\[ *[0-9]+\] +[0-9.]+-[0-9.]+ sec .* pps" | tail -1; done
rep=$($PYX --mhz $MHZ --counters | tee /dev/stderr | grep -o 'reports [0-9]*' | grep -o '[0-9]*$') 2>&1
# e5udp_host's verdict line says PROBLEM when fld_tx sent fewer frames than fld_rx received: here fld_tx sends only
# the server reports, so the check is fld_tx frames == iperf_srv's reports
touch $L/stop; endpoint_wait $L/host.log
tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host.log | grep -o '[0-9]*$')
echo "fld_tx frames ${tx:-?}, iperf_srv reports ${rep:-?}: $([ -n "$tx" ] && [ "$tx" = "$rep" ] && echo ACCOUNTED || echo "NOT ACCOUNTED")"
