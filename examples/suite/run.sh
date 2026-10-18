#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh <suite build dir> [example ...] -- the Vitis_Accel_Examples suite on the card, with the examples' own hosts.
#
# What it shows: each example of examples.txt (or the ones named) runs with the command its own makefile uses
#   (EXECUTABLE + CMD_ARGS of makefile_us_alveo.mk, $(BUILD_DIR) and $(EXECUTABLE) filled in; no CMD_ARGS: the first
#   xclbin of its build dir).
# Needs: the card running inn2x r10; the examples built under <suite build dir> (build.sh, build_hosts.sh); sudo only
#   to count kernel messages. The examples open XRT device 0, so on a host with several cards make sure device 0 is
#   the Innova-2 (xrt-smi examine). No cable, no DDR mode change.
# Run:   examples/suite/run.sh <suite build dir> [example ...]
#   SUITE_PROFILE=1  keep device_trace / device_counters in an example's xrt.ini; by default they are left out: with
#                    them, XRT 2.27 hosts pass and then crash on exit
# Prints: per example "<example> TEST PASSED | TEST FAILED | no verdict  rc=N" (output in run.log in its directory);
#   then "SUITE: P/N TEST PASSED; DMA failed +N, firewall trips +N" (kernel messages added during the run).
# PASS: P = N, DMA failed +0, firewall trips +0.
# Takes: depends on the selection; each example is limited to 600 s.

set -u
# 1. arguments, XRT, the example list; the kernel-message counts at the start
B=$(readlink -f "${1:?usage: $0 <suite build dir> [example ...]}"); shift
S=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
EXS=${*:-$(cat $S/examples.txt)}
cnt() { sudo -n dmesg 2>/dev/null | grep -cE "$1"; }
d0=$(cnt 'DMA failed'); f0=$(cnt 'Firewall 0 tripped'); p=0; n=0
# 2. each example: its EXECUTABLE and CMD_ARGS from the makefile, with the variables filled in
for ex in $EXS; do
  n=$((n+1)); d=$B/$ex; [ -d $d ] || { echo "$ex: not built"; continue; }
  cd $d; E=$(grep -m1 '^EXECUTABLE' makefile_us_alveo.mk | sed 's/.*=\s*//' | tr -d ' ')
  BD=$(ls -d build_dir.hw.* | head -1); A=$(grep -m1 '^CMD_ARGS' makefile_us_alveo.mk | sed 's/^[^=]*=\s*//')
  A=${A//\$(BUILD_DIR)/$BD}; A=${A//\$(EXECUTABLE)/$E}; A=$(eval echo "$A")
  [ -n "$A" ] || A="$BD/$(ls $BD | grep -m1 '\.xclbin$')"
  # its xrt.ini without device_trace / device_counters, unless SUITE_PROFILE=1
  unset XRT_INI_PATH
  if [ -f xrt.ini ] && [ "${SUITE_PROFILE:-0}" != 1 ]; then
    grep -vE '^\s*(device_trace|device_counters)\s*=' xrt.ini > xrt.suite.ini; export XRT_INI_PATH=$d/xrt.suite.ini
  fi
  # run it in its directory; the verdict is the last TEST PASSED / TEST FAILED line of its output
  timeout 600 $E $A > run.log 2>&1; rc=$?
  v=$(grep -aoiE "TEST PASSED|TEST FAILED" run.log | tail -1)
  case "$v" in *PASSED*) p=$((p+1));; esac
  printf '%-44s %-12s rc=%s\n' $ex "${v:-no verdict}" $rc
done
# 3. the total and the kernel messages this run added
echo "SUITE: $p/$n TEST PASSED; DMA failed +$(( $(cnt 'DMA failed') - d0 )), firewall trips +$(( $(cnt 'Firewall 0 tripped') - f0 ))"
