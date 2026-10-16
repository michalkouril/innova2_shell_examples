#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# hls4ml jet tagger (16 -> 64 -> 32 -> 32 -> 5, ap_fixed<16,6>, Resource strategy, reuse factor 32) inline in FLD-lite:
# each UDP request carries 16 features, the reply carries the 5 scores and the class. kernel/firmware/ is hls4ml's output
# (model/convert.py); h4ml_jet.cpp wraps it as a free-running stream kernel.
. "$(dirname "$0")/../../common/build_lib.sh"
begin hls4ml
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
K=$REPO/examples/hls4ml/kernel
xo h4ml_jet $K/h4ml_jet.cpp $K/firmware/jetnet.cpp -I$K -I$K/firmware
link hls4ml $REPO/examples/hls4ml/link.cfg Congestion_SpreadLogic_high
