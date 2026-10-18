#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_client.sh [xclbin] -- on the card's host: the FPGA as the iperf2 UDP *client* (fns iperf_udp, build_client.sh)
# sending from ConnectX port 0 (192.168.77.2:32768) to a stock `iperf -s -u` in the network namespace on port 1
# (192.168.77.1:5001). One experiment per datagram size (IPERF_WORDS, 64-byte words: default "23 128" = 1472 B and,
# with jumbo frames, 8192 B), each from a fresh xclbin load (fns's client state is not reset otherwise). fns's
# experiment is END_TIME_5s (781.25 M cycles) of its own clock (id 4, 50 MHz on r10: ~15.6 s). Judge the rate by the NIC's receive counter on port 1 and
# fld_tx's frame count: one iperf2 server socket receives only ~0.6-0.8 M datagrams/s, and fns sends its final
# datagram once, so iperf2's summary line can be off (use its per-second lines).
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/iperf_client.xclbin}"); L=$(mktemp -d "$D/run.XXXXXX")
preflight; netns_up 9000
NSX="sudo -n ip netns exec $NS"
nic() { $NSX ethtool -S $NET1 2>/dev/null | awk '/ rx_packets_phy:/{print $2}'; }
for wc in ${IPERF_WORDS:-23 128}; do
  echo "== fns iperf_udp: $wc x 64 B = $((wc * 64)) B datagrams, gap 0"
  sudo -n env PATH=$PATH xrt-smi program -d $USER_BDF -u /opt/xilinx/dsa/innova2_innova2_xdma_202320_4/test/verify.xclbin > /dev/null
  $NSX timeout 60 iperf -s -u -p 5001 -B $IPC -i 1 -l 16384 -w 8M > $L/srv_$wc.log 2>&1 &
  SRV=$!; sleep 1
  n0=$(nic)
  endpoint $L/host_$wc.log $X $IB0 $CX5_BDF --port 32768 --secs 40 --bf --jumbo --iperf $IPC,$wc,0,10 || { kill $SRV; exit 1; }
  # udp_tx_lite learns the server's MAC / IP from a datagram it receives: one hello to the FPGA's port 32768
  $NSX python3 -c "import socket; s=socket.socket(2,2); s.bind(('$IPC',0)); s.sendto(b'hello', ('$IPF',32768))"
  endpoint_wait $L/host_$wc.log 60 | grep -E 'fld_tx|E5UDP'
  n1=$(nic); wait $SRV 2>/dev/null
  grep -E 'iperf_udp started' $L/host_$wc.log
  grep -E '^\[ *[0-9]+\] +[0-9.]+-[0-9.]+ sec' $L/srv_$wc.log | head -12
  tx=$(grep -o 'fld_tx  (done): frames [0-9]*' $L/host_$wc.log | grep -o '[0-9]*$')
  echo "NIC port 1 rx_packets_phy: +$(( n1 - n0 )) frames; fld_tx sent ${tx:-?}"
done
