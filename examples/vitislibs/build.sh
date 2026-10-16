#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build.sh <benchmark> ... -- build AMD Vitis_Libraries benchmarks (paths as in benchmarks.txt, e.g.
# security/L1/benchmarks/crc32) for the Innova-2 with each benchmark's own Makefile ('make xclbin TARGET=hw'), in
# $BUILD/vitislibs/Vitis_Libraries. Every connectivity cfg of the benchmark is pinned to bank0 (pin_cfg.py); when the
# Makefile would not pass any cfg for this platform (they choose by platform name, e.g. 'u50'), the benchmark's DDR cfg
# (u250/u200, else u50) is passed through VPP_LDFLAGS. Then: common/link_gate.sh on the link, and every memory port must
# be on bank0 (suite/mem_check.py). Log: $BUILD/vitislibs/<benchmark with / -> _>.log; BUILD-OK / BUILD-FAIL per line.
# A benchmark under overlay/ (same path) is not AMD's as released: it is copied into the tree fresh, then built the same
# way (overlay/<path>/README.md says what it is). VL_JOBS benchmarks at a time (default 2). VL_CLEAN=1: after each
# benchmark, keep its reports and delete its _x_temp (clean_temp.sh; 1-3 GB each).
set -u
. "$(dirname "$0")/../../common/build_lib.sh"
S=$REPO/examples/vitislibs; L=$BUILD/vitislibs/Vitis_Libraries; mkdir -p $BUILD/vitislibs
# Vitis 2023.2's bundled g++ (v++ -c of the system-compiler "_sc" benchmarks, and sw_emu links) does not search the
# Ubuntu multiarch dirs: "bits/wordsize.h: No such file"
export CPATH=/usr/include/x86_64-linux-gnu${CPATH:+:$CPATH} LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}
EXS=${*:-$(grep -v '^#' $S/benchmarks.txt | awk '{print $1}')}
data=$(for b in $EXS; do awk -v b=$b '$1 == b && $2 == "data"' $S/benchmarks.txt | grep -q . && echo $b; done)
flock $BUILD/vitislibs/fetch.lock bash $S/fetch.sh $L $data >/dev/null || exit 1
# tests/ directories (not checked out) that a Makefile builds from, e.g. data_compression/L2/tests/src
tst=$(for b in $EXS; do [ -f $L/$b/Makefile ] && grep -oE '\$\(XFLIB_DIR\)/L[123]/tests/[A-Za-z0-9_]+' $L/$b/Makefile | sed "s#\$(XFLIB_DIR)#+${b%%/*}#"; done | sort -u)
[ -n "$tst" ] && { flock $BUILD/vitislibs/fetch.lock bash $S/fetch.sh $L $data $tst >/dev/null || exit 1; }
one() {
  local b=$1 slug; slug=$(echo $b | tr / _)
  ( set -u; d=$L/$b
    if [ -d $S/overlay/$b ]; then rm -rf $d; mkdir -p $d; cp -r $S/overlay/$b/. $d/   # ours (overlay/): a fresh copy
    else [ -f $d/Makefile ] && flock $BUILD/vitislibs/fetch.lock git -C $L checkout -q -- $b; fi   # pristine cfgs before pinning
    [ -f $d/Makefile ] || { echo "*** $b: no Makefile"; exit 2; }
    echo "== $(date +%T) $b"
    cd $d; rm -rf build_dir.hw.* _x_temp.hw.* reports .Xil
    for c in $(ls *.cfg 2>/dev/null); do grep -qE '^\s*(sp|nk)\s*=' $c && echo "   $c: $(python3 -B $S/pin_cfg.py $c) port(s) on bank0"; done
    n=$(python3 -B $S/pin_host.py --sys-port $d); [ "$n" != 0 ] && echo "   $n SYS_PORT(s) in the sources -> bank0"
    unset VPP_LDFLAGS
    if ! make -n xclbin TARGET=hw PLATFORM=$PFM 2>/dev/null | grep -E '\bv\+\+ .*-l\b|v\+\+ -l' | grep -q -- '--config'; then
      # only a cfg that defines the compute units (nk=): one with sp= lines alone may name CUs this platform's build
      # does not have (TreeEngine: conn_u200.cfg pins scanTreeKernel_2, the non-U250 build has one CU); sp_fill.py
      # below pins whatever is left
      c=$(for f in $(ls conn_u250.cfg conn_u200.cfg conn_u50.cfg conn*.cfg 2>/dev/null); do grep -qE '^\s*nk\s*=' $f && echo $f; done | head -1)
      [ -n "$c" ] && { export VPP_LDFLAGS="--config $d/$c"; echo "   no cfg for this platform in the Makefile: $c"; }
    fi
    # compile the kernels first: a link whose kernels need more URAM / LUTs than the region has left fails in placement
    # after 20-40 minutes (uram_check.py); stop before it
    dry=$(make -n -B xclbin TARGET=hw PLATFORM=$PFM 2>/dev/null)
    xos=$(echo "$dry" | grep -E '\bv\+\+ .*-c\b' | grep -oE "\-o '?[^ ']+\.xo" | sed -E "s/^-o '?//" | sort -u)
    cfgs=$(echo "$dry ${VPP_LDFLAGS:-}" | grep -oE -- '--config +[^ ]+' | awk '{print $2}' | sort -u)
    if [ -n "$xos" ]; then
      make $xos TARGET=hw PLATFORM=$PFM || { echo "*** $b: kernel compile failed"; exit 7; }
      python3 -B $S/uram_check.py ${VL_URAM_MAX:-32} $(ls -d _x_temp.hw.* | head -1) --lut-max ${VL_LUT_MAX:-300000} $cfgs ||
        { echo "*** $b: kernels need more URAM / LUTs than the dynamic region has left (VL_URAM_MAX=${VL_URAM_MAX:-32}, VL_LUT_MAX=${VL_LUT_MAX:-300000}); not linked"; exit 6; }
      # memory ports the cfgs leave unpinned would land in PLRAM0 (the platform default): pin them to bank0
      n=$(python3 -B $S/sp_fill.py $(ls -d _x_temp.hw.* | head -1) $d/innova2_sp.cfg $cfgs) || exit 5
      [ "$n" != 0 ] && { export VPP_LDFLAGS="${VPP_LDFLAGS:-} --config $d/innova2_sp.cfg"; echo "   $n unpinned memory port(s) -> bank0 (innova2_sp.cfg)"; }
    fi
    make xclbin TARGET=hw PLATFORM=$PFM; rc=$?
    X=$(ls build_dir.hw.*/*.xclbin 2>/dev/null | grep -v '\.link\.xclbin')
    T=$(ls -d _x_temp.hw.* 2>/dev/null | head -1)
    for xb in ${X:-none}; do
      bash $REPO/common/link_gate.sh $slug $rc /dev/null ${T:-none} $xb || exit 8
      bad=$(python3 -B $REPO/examples/suite/mem_check.py $xb bank0) || { echo "*** $xb: memory ports on $bad"; exit 9; }
    done
    echo "=== BUILD-OK $b" ) > $BUILD/vitislibs/$slug.log 2>&1
  grep -hE "BUILD-OK|\*\*\*" $BUILD/vitislibs/$slug.log | tail -1 | grep -q BUILD-OK && echo "$b BUILD-OK" || echo "$b BUILD-FAIL ($BUILD/vitislibs/$slug.log)"
  [ "${VL_CLEAN:-0}" = 1 ] && bash $S/clean_temp.sh $L/$b >> $BUILD/vitislibs/$slug.log 2>&1
}
J=${VL_JOBS:-2}
for b in $EXS; do while [ $(jobs -rp | wc -l) -ge $J ]; do wait -n; done; one $b & done; wait
