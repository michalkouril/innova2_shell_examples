#!/bin/bash
# link.sh <dir> <out.xclbin> [extra v++ args] -- link the .xo files + link.cfg in <dir> against $PLATFORM (an .xpfm)
# with the routing-congestion strategy (it routed the 12-port FLD-lite + generator design where the default did not).
# The routed timing summary is kept as timing_routed.rpt even when the link fails.
set -u
D=$1; OUTX=$2; shift 2; cd "$D" || exit 1
P=${PLATFORM:?set PLATFORM to the dev platform .xpfm}
FIX=${PEER_FIXUP:-/opt/xilinx/platforms/innova2_innova2_xdma_202320_4/tools/innova2_peer_fixup.sh}
rm -rf _x .ipcache .Xil link.xclbin "$OUTX"
v++ -l -t hw --platform "$P" --config link.cfg "$@" --vivado.prop run.impl_1.STRATEGY=Congestion_SpreadLogic_high \
    -o link.xclbin $(ls *.xo) > v++_link.log 2>&1; echo "link rc=$?"
grep -h "60-1230" v++_link.log | sed -E 's/.*following frequencies[^:]*: //'
R=$(find _x -name "*timing_summary_routed.rpt" | head -1); [ -n "$R" ] && cp "$R" timing_routed.rpt
[ -s link.xclbin ] && bash "$FIX" link.xclbin "$OUTX" && rm -rf _x .ipcache .Xil
