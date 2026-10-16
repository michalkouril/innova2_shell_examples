#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# FLD-lite line-rate echo: fld_rx -> afu_loop -> fld_tx (+ fld_reap, e5_ticker2) with the FPGA traffic generator / sink
# (gen_tx, sink_rx) on the same card. Routes with the congestion strategy.
. "$(dirname "$0")/../../common/build_lib.sh"
begin fld_echo
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
xo afu_loop $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl
for k in gen_tx sink_rx; do xo $k $F/gen/gen_sink.cpp; done
link fld_echo $REPO/examples/fld_echo/link.cfg Congestion_SpreadLogic_high
