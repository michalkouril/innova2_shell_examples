# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# net_lib_py.sh -- sourced by the network examples' run_py.sh scripts: net_lib.sh with the Python FLD-lite hosts
# (fld_lite/py: pyxrt + ctypes on the installed libibverbs / libmlx5, command layouts from OFED's mlx5_ifc.h) in
# place of the compiled ones. preflight and endpoint are redefined; everything else (netns_up, net_cleanup,
# endpoint_wait, the addresses) is net_lib.sh's. Still needed and not Python: the innova2_peermem kernel module (it
# lets the ConnectX register FPGA memory), loaded here if it is not loaded yet.
. "$(dirname "${BASH_SOURCE[0]}")/net_lib.sh" || exit 1
E5PY=$REPO/fld_lite/py
PYENV="PATH=$PATH PYTHONPATH=${PYTHONPATH:-} LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} XILINX_XRT=${XILINX_XRT:-}"
preflight() {
  [ "$(cat /sys/bus/pci/devices/$USER_BDF/p2p.u.*/p2p_enable 2>/dev/null)" = 1 ] || { echo "*** P2P is off on $USER_BDF (xrt-smi configure --p2p enable, warm reboot)"; exit 1; }
  python3 -c "import numpy, pyxrt" 2>/dev/null || { echo "*** python3 needs numpy and XRT's pyxrt (source /opt/xilinx/xrt/setup.sh)"; exit 1; }
  sudo -n env $PYENV python3 -c "import sys; sys.path.insert(0, '$E5PY'); import ifc, verbs; ifc.Ifc(); verbs.Verbs()" \
    || { echo "*** fld_lite/py: OFED's mlx5_ifc.h or libibverbs / libmlx5 not found"; exit 1; }
  if ! lsmod | grep -q "^innova2_peermem"; then
    K=$REPO/fld_lite/innova2_peermem/innova2_peermem.ko
    [ -s $K ] || { echo "*** innova2_peermem is not loaded and $K is not built (a kernel module: make -C fld_lite/innova2_peermem)"; exit 1; }
    sudo -n insmod $K || { echo "*** innova2_peermem"; exit 1; }
  fi
  [ -n "$NET1" ] || { echo "*** the ConnectX has no second port netdev"; exit 1; }
  # as net_lib.sh: a fresh load of the example's xclbin (verify.xclbin first), HYBRID DDR mode for the rings
  V=/opt/xilinx/dsa/innova2_innova2_xdma_202320_4/test/verify.xclbin
  [ -s $V ] || { echo "*** $V missing (innova2-xdma package)"; exit 1; }
  sudo -n env PATH=$PATH xrt-smi program -d $USER_BDF -u $V > /dev/null || { echo "*** loading $V failed"; exit 1; }
  sudo -n innova2_ddr_mode --bdf $MGMT_BDF hybrid | grep 'in force'
  DMA0=$(dmaf); trap net_cleanup EXIT
}
# endpoint <log> <xclbin> <args...>: e5udp_host.py on port 0 in the background (pid in $EP_PID); waits for it to come up
endpoint() {
  local log=$1; shift
  sudo -n bash -c "echo \$\$ > $log.pid; source /opt/xilinx/xrt/setup.sh >/dev/null; exec python3 -u $E5PY/e5udp_host.py $USER_BDF $* > $log 2>&1" < /dev/null &
  for w in $(seq 90); do grep -qE 'UP:|\*\*\*|Traceback|Error' $log 2>/dev/null && break; sleep 1; done
  EP_PID=$(cat $log.pid); grep -E 'UP:|\*\*\*|Traceback|Error' $log
  grep -q 'UP:' $log
}
