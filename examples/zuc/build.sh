#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# ZUC (128-EEA3 CONF / 128-EIA3 INTEG, 4-core FlexDriver AFU as an RTL kernel) inline in FLD-lite, plus the FPGA traffic
# generator / sink that drive it at line rate. v++ picks the data clock (about 210 MHz on ert4).
. "$(dirname "$0")/../../common/build_lib.sh"
begin zuc
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
for k in gen_tx sink_rx; do xo $k $F/gen/gen_sink.cpp; done
for k in zuc_in zuc_out; do xo $k $F/zuc/zuc_adapt.cpp; done
if [ ! -s zuc_k.xo ] || [ -n "$(find $F/zuc/rtl4 $F/zuc/package_zuc_k.tcl -newer zuc_k.xo)" ]; then
  echo "   package zuc_k (RTL, rtl4 = 4 cores)"; rm -rf zuc_pkg
  vivado -mode batch -nojournal -nolog -source $F/zuc/package_zuc_k.tcl -tclargs zuc_pkg zuc_k.v $F/zuc/rtl4 > pkg_zuc_k.log 2>&1 \
    || { echo "*** packaging zuc_k failed: $W/pkg_zuc_k.log"; exit 1; }
  cp zuc_pkg/zuc_k.xo zuc_k.xo
fi
link zuc $REPO/examples/zuc/link.cfg Congestion_SpreadLogic_high
