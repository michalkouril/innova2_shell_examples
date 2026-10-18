#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] -- the no-compile alternative to ../run.sh (the FPGA as an iperf2 UDP server): the same
# tests, checks and environment variables (see there), with FLD-lite set up by fld_lite/py/e5udp_host.py. Nothing on
# the data path is Python: only the set-up host changes.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
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
