#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh <root> [benchmark ...] -- on the card's host: run the Python (pyxrt) host of every benchmark of
# ../benchmarks.txt (or the ones named, by path or basename) that has one here (py/<basename>.py), on the prebuilt
# xclbin <root>/Vitis_Libraries/<benchmark path>/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin. Needs only
# python3, numpy, XRT's pyxrt (+ cryptography, the lz4 / zstd CLIs for some): no Vitis_Libraries checkout, no compiler.
# Prints each benchmark's result line and a total; PY_TIMEOUT (default 900 s) bounds each benchmark. Extra arguments
# for one script: PY_ARGS_<basename>="..." (e.g. PY_ARGS_crc32="--num 1024").
set -u
R=$(readlink -f "${1:?usage: $0 <root> [benchmark ...]}"); shift
P=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
ALL=$(grep -v '^#' $P/../benchmarks.txt | awk 'NF {print $1}')
SEL=${*:-$ALL}; n=0; pass=0; fail=""
for b in $SEL; do
  case $b in */*) path=$b ;; *) path=$(echo "$ALL" | awk -v b=$b -F/ '$NF == b' | head -1) ;; esac
  base=${path##*/}; s=$P/$base.py
  [ -n "$path" ] && [ -f $s ] || { [ $# -gt 0 ] && printf '%-48s %s\n' "$b" "no Python host"; continue; }
  x=$(ls $R/Vitis_Libraries/$path/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin 2>/dev/null | head -1)
  n=$((n+1))
  [ -n "$x" ] || { printf '%-48s %s\n' $path "FAIL: no xclbin under $R"; fail="$fail $base"; continue; }
  v=PY_ARGS_$base; out=$(timeout ${PY_TIMEOUT:-900} python3 $s $x ${!v:-} 2>&1); rc=$?
  line=$(echo "$out" | grep -E '^PYHOST-(PASS|FAIL)' | tail -1)
  if [ $rc = 0 ] && [[ $line == PYHOST-PASS* ]]; then pass=$((pass+1)); st=PASS; else st="FAIL rc=$rc"; fail="$fail $base"; fi
  printf '%-48s %s  %s\n' $path "$st" "${line#PYHOST-* }"
  [ "$st" = PASS ] || echo "$out" | tail -5 | sed 's/^/      /'
done
echo "VITISLIBS-PY: $pass/$n PASS${fail:+ (failed:$fail)}"
[ $pass = $n ]
