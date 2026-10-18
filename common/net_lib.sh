# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# net_lib.sh -- shared setup for the network examples (fld_echo, zuc, memcached, ntp, hls4ml, iperf). Sourced by their
# run.sh on the card's host, as a user with passwordless sudo (every privileged command is `sudo -n`).
#
# Needs: the card running inn2x r10 (ert4) with the innova2-xdma package installed; P2P enabled (`xrt-smi configure
#   --p2p enable`, then a warm reboot); OFED or DOCA-Host with DevX; `bash fld_lite/host_setup.sh` run once (the host
#   programs and innova2_peermem.ko); ConnectX-5 port 1 cabled back to port 0 with a DAC (a loop: the client or the
#   traffic generator on port 1 talks to the FPGA on port 0).
# Sets: everything card_env.sh exports, REPO (the repository), E5 (fld_lite/e5, the FLD-lite host programs), and
#   NS IPF IPC: the client's network namespace, the FPGA's IP on port 0, the client's IP on port 1.
# Functions:
#   preflight          the checks above; loads innova2_peermem; resets the kernels; sets HYBRID DDR mode; arranges for
#                      net_cleanup to run at exit
#   netns_up <mtu>     port 1 into namespace $NS with $IPC, $IPF on port 0 (why: see the function)
#   net_cleanup        undoes netns_up and the DDR mode; prints "DMA failed during this run: N" (N must be 0)
#   endpoint <log> <xclbin> <options>   starts e5udp_host, the FPGA's UDP endpoint, in the background ($EP_PID)
#   endpoint_wait <log> [seconds]       waits for it to end (default 200 s) and prints its counters and verdict

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$REPO/common/card_env.sh" || exit 1
E5=$REPO/fld_lite/e5
NS=innova2c; IPF=192.168.77.2; IPC=192.168.77.1        # FPGA endpoint on port 0, client in netns $NS on port 1
source /opt/xilinx/xrt/setup.sh >/dev/null
# dmaf: XRT's 'DMA failed' kernel messages since boot (net_cleanup prints how many this run added)
dmaf() { sudo -n dmesg | grep -c 'DMA failed'; }
preflight() {
  # 1. P2P must be on: the ConnectX-5 reaches the FPGA's memory (rings, frames, doorbells) through the P2P BAR
  [ "$(cat /sys/bus/pci/devices/$USER_BDF/p2p.u.*/p2p_enable 2>/dev/null)" = 1 ] || { echo "*** P2P is off on $USER_BDF (xrt-smi configure --p2p enable, warm reboot)"; exit 1; }
  # 2. the host programs; innova2_peermem (lets the ConnectX-5 register FPGA memory as an RDMA buffer); a second port
  for b in e5gen_host e5udp_host; do [ -x $E5/$b ] || { echo "*** $E5/$b missing: run fld_lite/host_setup.sh"; exit 1; }; done
  lsmod | grep -q "^innova2_peermem" || sudo -n insmod $REPO/fld_lite/innova2_peermem/innova2_peermem.ko || { echo "*** innova2_peermem"; exit 1; }
  [ -n "$NET1" ] || { echo "*** the ConnectX has no second port netdev"; exit 1; }
  # 3. reset the kernels:
  # The network kernels are free-running: they keep running (and keep any stuck state) after their host exits, and XRT
  # skips loading an xclbin whose UUID is already on the card. Load the shell's verify.xclbin first so the example's
  # xclbin always starts from a fresh load.
  V=/opt/xilinx/dsa/innova2_innova2_xdma_202320_4/test/verify.xclbin
  [ -s $V ] || { echo "*** $V missing (innova2-xdma package)"; exit 1; }
  sudo -n env PATH=$PATH xrt-smi program -d $USER_BDF -u $V > /dev/null || { echo "*** loading $V failed"; exit 1; }
  # 4. DDR mode; the DMA error count at the start; net_cleanup at exit
  # FLD-lite keeps its rings and RX slots in the 1 MB on-chip window: HYBRID DDR mode (restored to 'on' at exit)
  sudo -n innova2_ddr_mode --bdf $MGMT_BDF hybrid | grep 'in force'
  DMA0=$(dmaf); trap net_cleanup EXIT
}
# netns_up <mtu> (1500, or 9000 for jumbo frames): port 1 goes into its own network namespace. With both ports in one
#   namespace Linux would deliver the client's packets for the FPGA's IP locally, never through the cable; from $NS
#   they must leave port 1, cross the DAC and reach the FPGA on port 0. Port 0 also gets $IPF in Linux so that Linux
#   answers ARP for the FPGA's address (the FPGA handles only the UDP traffic to its port). Then 3 s to settle.
netns_up() {   # <mtu>
  for i in $NET0 $NET1; do sudo -n ip link set $i mtu $1 up; done
  sudo -n ip netns add $NS; sudo -n ip link set $NET1 netns $NS
  sudo -n ip netns exec $NS ip addr add $IPC/24 dev $NET1; sudo -n ip netns exec $NS ip link set $NET1 up
  sudo -n ip netns exec $NS ip link set lo up; sudo -n ip addr add $IPF/24 dev $NET0; sleep 3
}
# net_cleanup (at exit, from the trap): port 1 back to the main namespace, MTU 1500, DDR mode 'on', the DMA error count
net_cleanup() {
  if sudo -n ip netns list | grep -q "^$NS"; then
    sudo -n ip netns exec $NS ip link set $NET1 netns 1; sudo -n ip netns del $NS; sudo -n ip addr del $IPF/24 dev $NET0 2>/dev/null
  fi
  for i in $NET0 $NET1; do sudo -n ip link set $i mtu 1500 2>/dev/null; done
  sudo -n innova2_ddr_mode --bdf $MGMT_BDF on | grep 'in force'
  echo "DMA failed during this run: $(( $(dmaf) - DMA0 ))"
}
# endpoint <log> <xclbin> <args...>: e5udp_host on port 0 in the background (pid in $EP_PID); waits for it to come up
#   e5udp_host loads the xclbin, creates the ConnectX-5 queues on port 0, steers the UDP port to FLD-lite and starts the
#   engines; it runs for its --secs (or until its --stop-file appears). It runs as root under stdbuf -oL (line-buffered,
#   so the log can be read while it runs). Returns once the log shows "UP:" (ready) or an error, after at most 60 s;
#   non-zero if it did not come up.
endpoint() {
  local log=$1; shift
  sudo -n bash -c "echo \$\$ > $log.pid; source /opt/xilinx/xrt/setup.sh >/dev/null; exec stdbuf -oL $E5/e5udp_host $USER_BDF $* > $log 2>&1" < /dev/null &
  for w in $(seq 60); do grep -qE 'UP:|\*\*\*|what' $log 2>/dev/null && break; sleep 1; done
  EP_PID=$(cat $log.pid); grep -E 'UP:|\*\*\*|terminate|what' $log
  grep -q 'UP:' $log
}
# endpoint_wait <log> [seconds]: wait for e5udp_host to end, then print its FLD-lite counters and its verdict line
#   (E5UDP-HOST OK = engines stopped cleanly, no TX error completions, every accepted frame echoed)
endpoint_wait() { for w in $(seq ${2:-200}); do sudo -n kill -0 $EP_PID 2>/dev/null || break; sleep 1; done; grep -E 'E5UDP|fld_rx|fld_tx|fld_reap' $1; }
