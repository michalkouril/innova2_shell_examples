# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# card_env.sh -- find the Innova-2 card and its ConnectX-5. Sourced (`. common/card_env.sh`), not run, by the run
# scripts on the card's host; it only reads sysfs (no root). Prints one line,
#   card: FPGA user <BDF> mgmt <BDF> (<VBNV>, ROM_TS <n>); ConnectX-5 <port 0 BDF> (<RDMA dev> <netdev>) / <port 1 ...>
# and exports:
#   USER_BDF  MGMT_BDF   the FPGA's user / management PCI function (e.g. 0000:04:00.1 / 0000:04:00.0)
#   CX5_BDF   CX5_BDF1   the ConnectX-5's port 0 / port 1 PCI function
#   IB0       IB1        their RDMA devices (mlx5_N)
#   NET0      NET1       their network interfaces
# One card is found by itself: the Xilinx PCI function whose feature ROM names the r10 shell ($VBNV_WANT). With
# several, set USER_BDF=<user PF> first. The FPGA and the ConnectX-5 of one card sit behind the same on-board PCIe
# switch; that is how the ConnectX-5 that belongs to this FPGA is found. On an error it prints a "***" line and
# returns 1 (or exits 1 when run rather than sourced).

# the shell these examples are built for: inn2x r10 (ert4)
VBNV_WANT=innova2_innova2_xdma_202320_4
# 1. the FPGA: the one Xilinx (vendor 0x10ee) function whose ROM VBNV is $VBNV_WANT, unless USER_BDF is set
if [ -z "${USER_BDF:-}" ]; then
  c=$(for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor)" = 0x10ee ] && [ "$(cat $d/rom.u.*/VBNV 2>/dev/null)" = $VBNV_WANT ] && basename $d; done)
  [ "$(echo "$c" | grep -c .)" = 1 ] || { echo "*** found $(echo "$c" | grep -c .) Innova-2 user PFs running $VBNV_WANT ($c): set USER_BDF"; return 1 2>/dev/null || exit 1; }
  USER_BDF=$c
fi
[ -e /sys/bus/pci/devices/$USER_BDF ] || { echo "*** no PCI device $USER_BDF"; return 1 2>/dev/null || exit 1; }
# the management PF is function 0 of the same device
MGMT_BDF=${USER_BDF%.*}.0
# 2. the ConnectX-5: the Mellanox (0x15b3) functions with an RDMA device under the same PCIe switch, port 0 first
SW=$(dirname "$(dirname "$(readlink -f /sys/bus/pci/devices/$USER_BDF)")")      # the switch's upstream port
mapfile -t CX < <(for d in "$SW"/*/*/; do d=${d%/}; [ "$(cat $d/vendor 2>/dev/null)" = 0x15b3 ] && [ -d $d/infiniband ] && basename $d; done | sort)
[ ${#CX[@]} -ge 1 ] || { echo "*** no ConnectX-5 with an RDMA device next to $USER_BDF (OFED / DOCA-Host loaded?)"; return 1 2>/dev/null || exit 1; }
CX5_BDF=${CX[0]}; CX5_BDF1=${CX[1]:-}
# 3. their RDMA devices and network interfaces; export everything and print the summary line
IB0=$(ls /sys/bus/pci/devices/$CX5_BDF/infiniband); NET0=$(ls /sys/bus/pci/devices/$CX5_BDF/net 2>/dev/null | head -1)
IB1=; NET1=; [ -n "$CX5_BDF1" ] && { IB1=$(ls /sys/bus/pci/devices/$CX5_BDF1/infiniband); NET1=$(ls /sys/bus/pci/devices/$CX5_BDF1/net 2>/dev/null | head -1); }
export USER_BDF MGMT_BDF CX5_BDF CX5_BDF1 IB0 IB1 NET0 NET1
echo "card: FPGA user $USER_BDF mgmt $MGMT_BDF ($(cat /sys/bus/pci/devices/$USER_BDF/rom.u.*/VBNV), ROM_TS $(cat /sys/bus/pci/devices/$USER_BDF/rom.u.*/timestamp)); ConnectX-5 $CX5_BDF ($IB0 $NET0) / ${CX5_BDF1:-none} (${IB1:-} ${NET1:-})"
