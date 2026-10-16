# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# card_env.sh -- sourced on the card's host by the run scripts. Finds the Innova-2 and its ConnectX-5 and exports:
#   USER_BDF MGMT_BDF  FPGA user / management PF        CX5_BDF CX5_BDF1   ConnectX-5 port 0 / 1 PCI functions
#   IB0 IB1            RDMA devices of port 0 / 1       NET0 NET1          their netdevs
# Several cards: set USER_BDF (e.g. 0000:04:00.1) first. The FPGA and the ConnectX-5 of one card sit behind the same
# on-board PCIe switch, which is how the ConnectX is matched to the FPGA.
VBNV_WANT=innova2_innova2_xdma_202320_4
if [ -z "${USER_BDF:-}" ]; then
  c=$(for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor)" = 0x10ee ] && [ "$(cat $d/rom.u.*/VBNV 2>/dev/null)" = $VBNV_WANT ] && basename $d; done)
  [ "$(echo "$c" | grep -c .)" = 1 ] || { echo "*** found $(echo "$c" | grep -c .) Innova-2 user PFs running $VBNV_WANT ($c): set USER_BDF"; return 1 2>/dev/null || exit 1; }
  USER_BDF=$c
fi
[ -e /sys/bus/pci/devices/$USER_BDF ] || { echo "*** no PCI device $USER_BDF"; return 1 2>/dev/null || exit 1; }
MGMT_BDF=${USER_BDF%.*}.0
SW=$(dirname "$(dirname "$(readlink -f /sys/bus/pci/devices/$USER_BDF)")")      # the switch's upstream port
mapfile -t CX < <(for d in "$SW"/*/*/; do d=${d%/}; [ "$(cat $d/vendor 2>/dev/null)" = 0x15b3 ] && [ -d $d/infiniband ] && basename $d; done | sort)
[ ${#CX[@]} -ge 1 ] || { echo "*** no ConnectX-5 with an RDMA device next to $USER_BDF (OFED / DOCA-Host loaded?)"; return 1 2>/dev/null || exit 1; }
CX5_BDF=${CX[0]}; CX5_BDF1=${CX[1]:-}
IB0=$(ls /sys/bus/pci/devices/$CX5_BDF/infiniband); NET0=$(ls /sys/bus/pci/devices/$CX5_BDF/net 2>/dev/null | head -1)
IB1=; NET1=; [ -n "$CX5_BDF1" ] && { IB1=$(ls /sys/bus/pci/devices/$CX5_BDF1/infiniband); NET1=$(ls /sys/bus/pci/devices/$CX5_BDF1/net 2>/dev/null | head -1); }
export USER_BDF MGMT_BDF CX5_BDF CX5_BDF1 IB0 IB1 NET0 NET1
echo "card: FPGA user $USER_BDF mgmt $MGMT_BDF ($(cat /sys/bus/pci/devices/$USER_BDF/rom.u.*/VBNV), ROM_TS $(cat /sys/bus/pci/devices/$USER_BDF/rom.u.*/timestamp)); ConnectX-5 $CX5_BDF ($IB0 $NET0) / ${CX5_BDF1:-none} (${IB1:-} ${NET1:-})"
