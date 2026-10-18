#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh <root> [example ...] -- the suite examples again, each with a Python (pyxrt) host instead of its C++ one.
#
# What it shows: the kernels of ../run.sh, driven from Python: <example>.py in this directory (the example's path with
#   / -> _) runs on the prebuilt xclbin(s) <root>/<example>/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin -- the
#   layout examples/suite/build.sh leaves, or the prebuilt release unpacked.
# Needs: the card running inn2x r10; python3, numpy, XRT's pyxrt. No Vitis_Accel_Examples checkout, no compiler; sudo
#   only to count kernel messages. No cable, no DDR mode change.
# Run:   examples/suite/py/run_py.sh <root> [example ...]      (default: every example of ../examples.txt)
#   INNOVA2_DEV  the XRT device the examples open (BDF or index; default device 0)
# Prints: per example "<example> PASS|FAIL rc=N <seconds> s <result>" (after a FAIL also its last 5 output lines);
#   then "SUITE-PY: P/N PASS; DMA failed +N".
# PASS: P = N and DMA failed +0.
# Takes: depends on the selection; each example is limited to 600 s.

set -u
# 1. arguments, XRT, the example list; the 'DMA failed' count at the start
B=$(readlink -f "${1:?usage: $0 <root> [example ...]}"); shift
P=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
EXS=${*:-$(cat $P/../examples.txt)}
d0=$(sudo -n dmesg 2>/dev/null | grep -c 'DMA failed'); p=0; n=0
# 2. each example: its script and build dir; run it, timed (t in tenths of a second)
for ex in $EXS; do
  n=$((n+1)); s=$P/$(echo $ex | tr / _).py; d=$B/$ex/build_dir.hw.innova2_innova2_xdma_202320_4
  [ -d $d ] || { printf '%-44s %s\n' $ex "no build_dir"; continue; }
  t0=$(date +%s%N); out=$(timeout 600 python3 -B $s $d 2>&1); rc=$?; t=$(( ($(date +%s%N) - t0) / 100000000 ))
  # the verdict is the script's last PYHOST-PASS / PYHOST-FAIL line
  v=$(echo "$out" | grep -aE '^PYHOST-(PASS|FAIL)' | tail -1)
  case "$v" in PYHOST-PASS*) p=$((p+1)); r=PASS;; *) r=FAIL; echo "$out" | tail -5 | sed 's/^/    /';; esac
  printf '%-44s %-5s rc=%s %4s s  %s\n' $ex $r $rc $((t / 10)).$((t % 10)) "$(echo "$v" | sed 's/^PYHOST-[A-Z]* [^:]*:\? \?//')"
done
# 3. the total
echo "SUITE-PY: $p/$n PASS; DMA failed +$(( $(sudo -n dmesg 2>/dev/null | grep -c 'DMA failed') - d0 ))"
