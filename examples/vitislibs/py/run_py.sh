#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh <root> [benchmark ...] -- Vitis Libraries benchmarks with Python (pyxrt) hosts instead of AMD's C++ ones.
#
# What it shows: every benchmark of ../benchmarks.txt (or the ones named, by path or by basename) that has a Python
#   host here (py/<basename>.py) runs on its prebuilt xclbin,
#   <root>/Vitis_Libraries/<benchmark path>/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin.
# Needs: the card running inn2x r10; python3, numpy, XRT's pyxrt (+ cryptography and the lz4 / zstd command-line tools
#   for some). No Vitis_Libraries checkout, no compiler, no root, no cable, no DDR mode change.
# Run:   examples/vitislibs/py/run_py.sh <root> [benchmark ...]
#   PY_TIMEOUT          seconds per benchmark (default 900)
#   PY_ARGS_<basename>  extra arguments for one script, e.g. PY_ARGS_crc32="--num 1024"
# Prints: per benchmark "<path> PASS|FAIL rc=N <result line>" (after a FAIL also its last 5 output lines); then
#   "VITISLIBS-PY: P/N PASS" and the ones that failed.
# PASS: P = N (exit code 0).
# Takes: depends on the selection; each benchmark is limited to PY_TIMEOUT.

set -u
# 1. arguments, XRT, the benchmark list
R=$(readlink -f "${1:?usage: $0 <root> [benchmark ...]}"); shift
P=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
ALL=$(grep -v '^#' $P/../benchmarks.txt | awk 'NF {print $1}')
SEL=${*:-$ALL}; n=0; pass=0; fail=""
# 2. each benchmark: its full path (a basename is looked up in benchmarks.txt), its script, its xclbin. Without
#    arguments a benchmark with no Python host is skipped silently; a named one is reported
for b in $SEL; do
  case $b in */*) path=$b ;; *) path=$(echo "$ALL" | awk -v b=$b -F/ '$NF == b' | head -1) ;; esac
  base=${path##*/}; s=$P/$base.py
  [ -n "$path" ] && [ -f $s ] || { [ $# -gt 0 ] && printf '%-48s %s\n' "$b" "no Python host"; continue; }
  x=$(ls $R/Vitis_Libraries/$path/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin 2>/dev/null | head -1)
  n=$((n+1))
  [ -n "$x" ] || { printf '%-48s %s\n' $path "FAIL: no xclbin under $R"; fail="$fail $base"; continue; }
  # run it (${!v} = the value of PY_ARGS_<basename>); PASS = exit code 0 and a PYHOST-PASS line
  v=PY_ARGS_$base; out=$(timeout ${PY_TIMEOUT:-900} python3 $s $x ${!v:-} 2>&1); rc=$?
  line=$(echo "$out" | grep -E '^PYHOST-(PASS|FAIL)' | tail -1)
  if [ $rc = 0 ] && [[ $line == PYHOST-PASS* ]]; then pass=$((pass+1)); st=PASS; else st="FAIL rc=$rc"; fail="$fail $base"; fi
  printf '%-48s %s  %s\n' $path "$st" "${line#PYHOST-* }"
  [ "$st" = PASS ] || echo "$out" | tail -5 | sed 's/^/      /'
done
# 3. the total; the exit code is 0 only if every benchmark passed
echo "VITISLIBS-PY: $pass/$n PASS${fail:+ (failed:$fail)}"
[ $pass = $n ]
