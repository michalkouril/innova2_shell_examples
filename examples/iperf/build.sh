#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# iperf2 UDP server inline in FLD-lite (kernels/iperf_kernel.cpp): fld_rx -> iperf_srv -> fld_tx counts the host's
# `iperf -c <ip> -u` datagrams and answers its final datagram with iperf2's server report; iperf_tmr is its clock,
# iperf_sctl (host-started) sets the port and reads the counters.
. "$(dirname "$0")/../../common/build_lib.sh"
begin iperf
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
K=$REPO/examples/iperf/kernels
for k in iperf_tmr iperf_srv iperf_sctl; do xo $k $K/iperf_kernel.cpp; done
link iperf $REPO/examples/iperf/link.cfg
