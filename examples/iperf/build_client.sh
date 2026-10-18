#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# The iperf2 UDP *client* in the FPGA: ETH Zurich fpga-network-stack's iperf_udp (fetched by fetch_fns.sh, compiled
# unchanged) wrapped as kernels/iperf_fns.cpp on FLD-lite + udp_rx_lite / udp_tx_lite; it sends to a stock
# `iperf -s -u` on the host. fns counts its experiment in its own clock: the three fns kernels run on the platform's
# free-running clock (id 4; 50 MHz on r10), as on the shell they were validated on.
. "$(dirname "$0")/../../common/build_lib.sh"
begin iperf_client
F=$REPO/fld_lite/e5; K=$REPO/examples/iperf/kernels; FNS=$W/fpga-network-stack
bash $REPO/examples/iperf/fetch_fns.sh $FNS || exit 1
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
for k in udp_rx_lite udp_tx_lite; do xo $k $F/udp_lite.cpp; done
for k in iperf_udp_fns iperf_ctl iperf_clock; do xo $k $K/iperf_fns.cpp -I$FNS/hls --hls.clock 200000000:$k; done
link iperf_client $REPO/examples/iperf/link_client.cfg
