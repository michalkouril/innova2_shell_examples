#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build.sh <Vitis_Libraries dir> <out dir> [HLS include dir] -- build the graph-input tools into <out dir>/bin (the
# prepare.sh out dir): the converter (mtx2csr),
# the CPU references (sssp_ref, twohop_ref, tc_ref) and AMD's kernel code compiled as plain C++ (sssp_csim, tc_csim,
# lp_csim: HLS C-simulation semantics; they need the library's graph / quantitative_finance / utils headers and the
# Vitis HLS ap_int / hls_stream headers, default $XILINX_HLS/include).
set -eu
L=$(readlink -f "${1:?usage: $0 <Vitis_Libraries dir> <out dir> [HLS include dir]}"); O=$(mkdir -p "${2:?out dir}" && readlink -f "$2")
H=${3:-${XILINX_HLS:-/nonexistent}/include}
[ -s "$H/ap_int.h" ] || { echo "*** no ap_int.h in $H"; exit 2; }
D=$(cd "$(dirname "$0")" && pwd); T=$D/tools; B=$O/bin; mkdir -p $B
for c in mtx2csr sssp_ref twohop_ref tc_ref; do gcc -O2 -o $B/$c $T/$c.c -lm; done   # no -ffast-math: IEEE float sums
INC="-I$T -I$H -I$L/graph/L2/include -I$L/graph/L2/include/hw -I$L/quantitative_finance/L1/include -I$L/database/L1/include/hw -I$L/utils/L1/include"
for c in sssp_csim tc_csim lp_csim; do g++ -O2 -std=c++14 -w $INC -o $B/$c $T/$c.cpp; done
ls $B
