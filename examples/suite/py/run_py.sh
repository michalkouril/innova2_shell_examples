#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh <root> [example ...] -- on the card's host: run the Python (pyxrt) host of each suite example on its
# prebuilt xclbin(s), <root>/<example>/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin (the layout
# examples/suite/build.sh leaves, or the prebuilt release unpacked). No Vitis_Accel_Examples checkout and no compiler
# needed: python3, numpy, XRT's pyxrt. The examples open XRT device 0 unless INNOVA2_DEV names another (BDF or index).
# Prints PASS / FAIL per example and a total; also the number of 'DMA failed' kernel messages during the run.
set -u
B=$(readlink -f "${1:?usage: $0 <root> [example ...]}"); shift
P=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
EXS=${*:-$(cat $P/../examples.txt)}
d0=$(sudo -n dmesg 2>/dev/null | grep -c 'DMA failed'); p=0; n=0
for ex in $EXS; do
  n=$((n+1)); s=$P/$(echo $ex | tr / _).py; d=$B/$ex/build_dir.hw.innova2_innova2_xdma_202320_4
  [ -d $d ] || { printf '%-44s %s\n' $ex "no build_dir"; continue; }
  t0=$(date +%s%N); out=$(timeout 600 python3 -B $s $d 2>&1); rc=$?; t=$(( ($(date +%s%N) - t0) / 100000000 ))
  v=$(echo "$out" | grep -aE '^PYHOST-(PASS|FAIL)' | tail -1)
  case "$v" in PYHOST-PASS*) p=$((p+1)); r=PASS;; *) r=FAIL; echo "$out" | tail -5 | sed 's/^/    /';; esac
  printf '%-44s %-5s rc=%s %4s s  %s\n' $ex $r $rc $((t / 10)).$((t % 10)) "$(echo "$v" | sed 's/^PYHOST-[A-Z]* [^:]*:\? \?//')"
done
echo "SUITE-PY: $p/$n PASS; DMA failed +$(( $(sudo -n dmesg 2>/dev/null | grep -c 'DMA failed') - d0 ))"
