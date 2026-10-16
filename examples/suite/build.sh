#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build.sh [example ...] -- build the 20-example Vitis suite (AMD Vitis_Accel_Examples, MIT, pinned commit) for the
# Innova-2: copies each example to $BUILD/suite/<example>, re-pins every memory port to bank0 (prep_cfg.py; the stock
# cfgs name DDR[n]/HBM[n]), runs the example's own 'make all TARGET=hw', and checks that every memory port landed on
# bank0. SUITE_JOBS examples at a time (default 2). Logs: $BUILD/suite/<example>.log. Not part of build_all.sh.
set -u
. "$(dirname "$0")/../../common/build_lib.sh"
S=$REPO/examples/suite; UP=$BUILD/suite/Vitis_Accel_Examples; RUN=$BUILD/suite
mkdir -p $RUN; flock $RUN/fetch.lock bash $S/fetch_vitis_accel_examples.sh $UP || exit 1
export VMEM=bank0
one() {
  local ex=$1 slug; slug=$(echo $ex | tr / _)
  ( set -u; d=$RUN/$ex; rm -rf $d; mkdir -p $(dirname $d); cp -r $UP/$ex $d && cd $d || exit 2
    echo "== $(date +%T) $ex"
    python3 $S/prep_cfg.py . | tee .prep.out || { echo "*** prep_cfg"; exit 3; }
    J=-j4; n=$(sed -n 's/.*containers: \([0-9]*\).*/\1/p' .prep.out | tail -1)
    { [ "${n:-1}" -gt 1 ] || [ $(ls scripts/package_kernel_*.tcl 2>/dev/null | wc -l) -gt 1 ]; } && J=-j1   # shared temp dirs: serial
    make all TARGET=hw PLATFORM="$PFM" COMMON_REPO="$UP/" $J; rc=$?
    X=$(find . -name '*.xclbin' ! -name '*.link.xclbin' | head -5)
    if [ $rc = 0 ] && [ -n "$X" ]; then
      for xb in $X; do bad=$(python3 $S/mem_check.py $xb bank0) || { echo "*** $xb: memory ports on $bad"; exit 9; }; done
      rm -rf _x.hw.* .Xil; echo "=== BUILD-OK $ex"
    else echo "=== BUILD-FAIL $ex (rc=$rc)"; exit 1; fi ) > $RUN/$slug.log 2>&1
  grep -hE "BUILD-OK|BUILD-FAIL|\*\*\*" $RUN/$slug.log | tail -1
}
EXS=${*:-$(cat $S/examples.txt)}; J=${SUITE_JOBS:-2}
for ex in $EXS; do while [ $(jobs -rp | wc -l) -ge $J ]; do wait -n; done; one $ex & done; wait
for ex in $EXS; do printf '%-44s %s\n' $ex "$(grep -qs BUILD-OK $RUN/$(echo $ex | tr / _).log && echo BUILD-OK || echo BUILD-FAIL)"; done
