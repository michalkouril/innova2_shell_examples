#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_client_py.sh [xclbin] -- the no-compile alternative to ../run_client.sh (the FPGA, fns iperf_udp, as the
# iperf2 UDP client): the same tests, checks and environment variables (see there), with FLD-lite set up by
# fld_lite/py/e5udp_host.py --iperf. Nothing on the data path is Python: only the set-up host changes.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
X=$(readlink -f "${1:-$REPO/xclbin/iperf_client.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
# 2. checks, kernel reset, HYBRID DDR mode; port 1 into the namespace, both ports at MTU 9000
preflight; netns_up 9000
# NSX runs a command in the namespace; nic = the NIC's hardware receive counter on port 1 (where the server is)
NSX="sudo -n ip netns exec $NS"
nic() { $NSX ethtool -S $NET1 2>/dev/null | awk '/ rx_packets_phy:/{print $2}'; }
# 3. one experiment per datagram size
for wc in ${IPERF_WORDS:-23 128}; do
  echo "== fns iperf_udp: $wc x 64 B = $((wc * 64)) B datagrams, gap 0"
  # fresh state: verify.xclbin first, so the endpoint below loads the client xclbin anew
  sudo -n env PATH=$PATH xrt-smi program -d $USER_BDF -u /opt/xilinx/dsa/innova2_innova2_xdma_202320_4/test/verify.xclbin > /dev/null
  # the stock server in the namespace, for at most 60 s
  $NSX timeout 60 iperf -s -u -p 5001 -B $IPC -i 1 -l 16384 -w 8M > $L/srv_$wc.log 2>&1 &
  SRV=$!; sleep 1
  n0=$(nic)
  # the endpoint on port 32768 (40 s at most); --iperf IP,WORDS,GAP,DELAY_S starts fns's client 10 s after it is up,
  # sending WORDS x 64 B datagrams to IP with GAP 0
  endpoint $L/host_$wc.log $X $IB0 $CX5_BDF --port 32768 --secs 40 --bf --jumbo --iperf $IPC,$wc,0,10 || { kill $SRV; exit 1; }
  # udp_tx_lite learns the server's MAC / IP from a datagram it receives: one hello to the FPGA's port 32768
  $NSX python3 -c "import socket; s=socket.socket(2,2); s.bind(('$IPC',0)); s.sendto(b'hello', ('$IPF',32768))"
  # wait for the endpoint to end; then the counters and the server's per-second lines
  endpoint_wait $L/host_$wc.log 60 | grep -E 'fld_tx|E5UDP'
  n1=$(nic); wait $SRV 2>/dev/null
  grep -E 'iperf_udp started' $L/host_$wc.log
  grep -E '^\[ *[0-9]+\] +[0-9.]+-[0-9.]+ sec' $L/srv_$wc.log | head -12
  tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host_$wc.log | grep -o '[0-9]*$')
  echo "NIC port 1 rx_packets_phy: +$(( n1 - n0 )) frames; fld_tx sent ${tx:-?}"
done
