#!/bin/bash

# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# link_gate.sh -- verdict on one v++ link, so a link that failed routing or lowered a clock cannot pass silently.
#   link_gate.sh <name> <rc> <v++ stdout log> <v++ --temp_dir> <xclbin> [want DATA_CLK MHz] [want KERNEL_CLK MHz]
# Fails (exit 1) on: nonzero rc, no xclbin, VPL 18-1000 (partially-conflicted nets), an illegally routed design
# (node overlaps / nets that failed to route), VPL 101-2 (an unscalable clock -- XDMA axi_aclk id 3, freerun id 4 --
# missed timing), or a kernel clock that v++ selected below the wanted frequency (60-1230). Always prints the routing
# overlap / failed-net summary, the congestion levels, and the per-clock WNS/WHS of the routed link.
# clk-alveo-300-fp: added after the 300 MHz shell's own verify link failed routing (1,078 overlaps) inside build.sh.
set -u
N=$1; RC=$2; LOG=$3; TMP=$4; XB=$5; W0=${6:-}; W1=${7:-}
# the live impl log while v++ keeps the project (a failed link); v++ copies it to logs/link/imp/ when the link succeeds
RL=$TMP/link/vivado/vpl/prj/prj.runs/impl_1/runme.log; [ -s "$RL" ] || RL=$TMP/logs/link/imp/impl_1_runme.log
bad=0; say(){ echo "LINK-GATE $N $*"; }
[ "$RC" = 0 ] || { say "*** v++ exit status $RC"; bad=1; }
[ -s "$XB" ] || { say "*** no xclbin $XB"; bad=1; }
if [ -s "$RL" ]; then
  ov=$(grep -aoE "There are [0-9]+ node overlaps" "$RL" | tail -1 | grep -oE "[0-9]+")
  fl=$(grep -aoE "[0-9]+ signals failed to route" "$RL" | tail -1 | grep -oE "^[0-9]+")
  lv=$(grep -aoE "Effective congestion level: [0-9]+" "$RL" | grep -oE "[0-9]+$" | sort -n | tail -1)
  hc=$(grep -ac "Place 46-14" "$RL")
  # router's initial estimated congestion (Route 35-449): the largest congested window NxN -> level log2(N)
  il=$(awk '/Route 35-449/{f=1;n=0} f{n++; if(n>16) f=0; while (match($0, /[0-9]+x[0-9]+/)) { s=substr($0, RSTART, RLENGTH); split(s, a, "x"); if (a[1]+0 > m) m=a[1]+0; $0=substr($0, RSTART+RLENGTH) } } END{l=0; while (m>1) { m/=2; l++ }; print l}' "$RL")
  say "route: node overlaps=${ov:-0} failed nets=${fl:-0}; initial estimated congestion level=$il; final effective congestion level=${lv:-none reported}; placer 'highly congested' warnings=$hc"
  grep -aE "Router Estimated|INT_X[0-9]+Y[0-9]+->INT_X" "$RL" | head -4 | sed "s/^/LINK-GATE $N   congestion window: /"
  [ -n "$ov" ] && [ "$ov" != 0 ] && { say "*** design is not legally routed ($ov node overlaps)"; bad=1; }
  [ -n "$fl" ] && [ "$fl" != 0 ] && { say "*** $fl nets failed to route"; bad=1; }
else
  say "*** no implementation log $RL"; bad=1
fi
if grep -aq "VPL 18-1000" "$LOG"; then
  say "*** VPL 18-1000 partially-conflicted nets:"; grep -a "VPL 18-1000" "$LOG" | head -1 | cut -c1-600 | sed "s/^/LINK-GATE $N     /"; bad=1
fi
grep -aq "VPL 101-2" "$LOG" && { say "*** VPL 101-2: an unscalable clock (axi_aclk id 3 / freerun id 4) missed timing"; bad=1; }
# fp8c: a platform that ships tcl_hooks/rp_crossings.xdc (RP-side pblocks for the host-memory / PEER converters) must
# have it parsed in this link's implementation with no critical warning about it -- otherwise the floorplan that keeps
# the fixed 250 MHz paths of host_cc / peer_cc short silently did not apply.
HK=$TMP/link/vivado/vpl/.local/hw_platform/tcl_hooks/rp_crossings.xdc
if [ -e "$HK" ]; then
  np=$(grep -ac "Parsing XDC File.*rp_crossings.xdc" "$RL" 2>/dev/null); cw=$(grep -a "CRITICAL WARNING" "$RL" 2>/dev/null | grep -c "rp_crossings.xdc")
  say "rp_crossings.xdc hook: parsed ${np:-0}x, critical warnings ${cw:-0}"
  { [ "${np:-0}" -ge 1 ] && [ "${cw:-0}" = 0 ]; } || { say "*** rp_crossings.xdc hook not applied cleanly"; bad=1; }
fi
F=$(grep -a "v++ 60-1230" "$LOG" | tail -1)
f0=$(echo "$F" | grep -oE "clkwiz_kernel_clk_out1 = [0-9.]+" | grep -oE "[0-9.]+$")
f1=$(echo "$F" | grep -oE "clkwiz_kernel2_clk_out1 = [0-9.]+" | grep -oE "[0-9.]+$")
say "clocks selected: DATA_CLK(id0)=${f0:-?} KERNEL_CLK(id1)=${f1:-?} (want ${W0:-any}/${W1:-any})"
if [ -n "$W0" ] && { [ -z "$f0" ] || python3 -c "import sys; sys.exit(0 if float('$f0') < float('$W0') - 0.5 else 1)"; }; then say "*** DATA_CLK lowered or unknown"; bad=1; fi
if [ -n "$W1" ] && { [ -z "$f1" ] || python3 -c "import sys; sys.exit(0 if float('$f1') < float('$W1') - 0.5 else 1)"; }; then say "*** KERNEL_CLK lowered or unknown"; bad=1; fi
python3 - "$TMP" "$N" <<'PY' || true
import glob, re, sys, os
d, n = sys.argv[1], sys.argv[2]
rs = sorted(glob.glob(d + '/reports/link/imp/impl_1_*timing_summary_postroute_physopted.rpt')) or \
     sorted(glob.glob(d + '/reports/link/imp/impl_1_*timing_summary_routed.rpt')) or \
     sorted(glob.glob(d + '/link/vivado/vpl/prj/prj.runs/impl_1/*timing_summary*routed.rpt'))
if not rs: print('LINK-GATE %s timing: no routed timing summary' % n); sys.exit(0)
t = open(rs[0]).read()
# The report has no pins, so clocks are matched by name. axi_aclk's name follows a net it drives
# (mgmt_rom_ctrl_BRAM_PORTA_CLK; the ertfit trial made it ert_cq_host_BRAM_PORTA_CLK), so it is matched as *_BRAM_PORTA_CLK.
# A label that matches nothing is printed as missing instead of being dropped.
names = [(lambda c: c == 'clk_wiz_kernel_clk_raw', 'DATA_CLK(id0)'), (lambda c: c == 'clk_wiz_kernel2_clk_raw', 'KERNEL_CLK(id1)'),
         (lambda c: c.endswith('_BRAM_PORTA_CLK'), 'axi_aclk(id3)'), (lambda c: c == 'clk_out1_pfm_top_clk_wiz_0_0', 'freerun(id4)')]
m = re.search(r'\| Design Timing Summary\n.*?\n\s+WNS.*?\n\s+-+.*?\n\s+(\S+)\s+\S+\s+\S+\s+\S+\s+(\S+)', t, re.S)
out = 'overall WNS=%s WHS=%s' % (m.group(1), m.group(2)) if m else 'overall ?'
sec = t.split('| Intra Clock Table')[1].split('| Inter Clock Table')[0] if '| Intra Clock Table' in t else ''
for want, label in names:
    hits = [f for f in (l.split() for l in sec.splitlines()) if len(f) > 5 and want(f[0])]
    if len(hits) == 1: out += '; %s WNS=%s WHS=%s' % (label, hits[0][1], hits[0][5])
    else: out += '; %s %s' % (label, 'not in the report' if not hits else 'ambiguous: ' + ','.join(f[0] for f in hits))
print('LINK-GATE %s timing (%s): %s' % (n, os.path.basename(rs[0]), out))
PY
[ $bad = 0 ] && say "PASS" || say "FAIL"
exit $bad
