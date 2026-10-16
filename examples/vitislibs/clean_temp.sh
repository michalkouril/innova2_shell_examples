#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# clean_temp.sh <benchmark dir> ... -- free the 1-3 GB a finished benchmark build leaves in <dir>/_x_temp.hw.*: first
# copy what explains the result into <dir>/innova2_reports/ (routed timing summary and utilization, the kernels'
# synthesis utilization, the link logs, each kernel's top-level HLS report and vitis_hls.log), then delete _x_temp --
# only when something was copied. build.sh runs it after each benchmark when VL_CLEAN=1. System-compiler benchmarks'
# kernel objects are kept in innova2_reports/sc_objs/ (their host links against them).
set -u
for d in "$@"; do
  T=$(ls -d $d/_x_temp.hw.* 2>/dev/null | head -1); [ -n "$T" ] || continue
  R=$d/innova2_reports; mkdir -p $R/hls $R/link
  find $T -path '*impl_1*' \( -name '*timing_summary_routed.rpt' -o -name '*utilization_placed.rpt' \
       -o -name 'kernel_util_routed.xutil' \) -exec cp -t $R/link {} + 2>/dev/null
  find $T -name '*utilization_synth.rpt' -path '*pfm_dynamic_*' -exec cp -t $R/link {} + 2>/dev/null
  [ -d $T/logs/link ] && cp -r $T/logs/link $R/link/logs
  find $T -path '*/solution/syn/report/*_csynth.rpt' | awk -F/ '{k=$NF; sub(/_csynth.rpt/,"",k); if ($(NF-6)==k) print}' |
    sort -u | xargs -r cp -t $R/hls
  find $T -maxdepth 3 -name vitis_hls.log | while read f; do cp $f $R/hls/$(basename $(dirname $f))_vitis_hls.log; done
  # a system-compiler (_sc) benchmark links its host against the kernels' v++ -c objects (<temp>/kernel/*.o, next to
  # build_dir/*.o): keep them, or the host cannot be built without Vitis
  if ls $d/build_dir.hw.*/*.o >/dev/null 2>&1; then
    for o in $(cd $T && find . -path './kernel/*.o'); do mkdir -p $R/sc_objs/$(dirname $o); cp $T/$o $R/sc_objs/$o; done; fi
  n=$(find $R -type f | wc -l)
  if [ $n -gt 0 ]; then rm -rf "$T"; echo "   cleaned: kept $n report files in innova2_reports/, removed $(basename $T)"
  else echo "   not cleaned: nothing to keep from $(basename $T)"; fi
done
