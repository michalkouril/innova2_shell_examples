#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# host_setup.sh -- build the FLD-lite host programs and the peer-memory module, once, on the card's host.
#
# Builds: fld_lite/e5/{e5gen_host,e5udp_host,e5_host} (the DevX hosts of the network examples), fld_lite/lat/lat_host
#   (lat_probe's host) and fld_lite/innova2_peermem/innova2_peermem.ko (lets the ConnectX-5 use FPGA memory as an
#   RDMA buffer; the run scripts load it).
# Needs: XRT, OFED with DevX (MLNX_OFED or DOCA-Host), rdma-core headers, kernel headers, g++, make. No root.
#   mlx5_ifc.h / mlx5_ifc_fpga.h (dual GPL-2.0 / BSD, not in this repository) are copied from OFED's source tree into
#   fld_lite/devx/; MLX5_IFC_DIR=<OFED's include/linux/mlx5> says where they are when the default is wrong.
# Run:   bash fld_lite/host_setup.sh
# Prints: the size and path of the five files built, then HOST-SETUP-OK; it stops at the first error.
# Takes: under a minute.

set -eu
H=$(cd "$(dirname "$0")" && pwd)
# 1. the ConnectX-5 interface headers, from OFED
IFC=${MLX5_IFC_DIR:-$(ls -d /usr/src/ofa_kernel/*/$(uname -r)/include/linux/mlx5 2>/dev/null | head -1)}
[ -s "$IFC/mlx5_ifc.h" ] || { echo "*** mlx5_ifc.h not found (set MLX5_IFC_DIR to OFED's include/linux/mlx5)"; exit 1; }
cp "$IFC/mlx5_ifc.h" "$IFC/mlx5_ifc_fpga.h" "$H/devx/"
# 2. the host programs (XRT + rdma-core)
source /opt/xilinx/xrt/setup.sh >/dev/null
make -s -C "$H/e5" -f Makefile.host e5gen_host e5udp_host e5_host
g++ -std=c++17 -O2 -o "$H/lat/lat_host" "$H/lat/lat_host.cpp" -I$XILINX_XRT/include -L$XILINX_XRT/lib -lxrt_coreutil -pthread -Wl,-rpath,$XILINX_XRT/lib
# 3. the peer-memory module
# innova2_peermem: against OFED's ib_core (its Makefile picks /usr/src/ofa_kernel/<arch>/<kernel>)
make -s -C "$H/innova2_peermem" >/dev/null
# 4. what was built
ls -la "$H/e5/e5gen_host" "$H/e5/e5udp_host" "$H/e5/e5_host" "$H/lat/lat_host" "$H/innova2_peermem/innova2_peermem.ko" | awk '{print $5, $NF}'
echo HOST-SETUP-OK
