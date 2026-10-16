#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh <suite build dir> [example ...] -- on the card's host: run each built example with the command its own makefile
# uses (EXECUTABLE + CMD_ARGS of makefile_us_alveo.mk). The examples open XRT device 0, so on a host with several cards
# make sure device 0 is the Innova-2 (xrt-smi examine). Prints TEST PASSED / FAILED per example and a total; also the
# number of 'DMA failed' and firewall-trip kernel messages during the run. An example's xrt.ini is used without
# device_trace / device_counters (SUITE_PROFILE=1 keeps them): with those, XRT 2.27 hosts pass and then crash on exit.
set -u
B=$(readlink -f "${1:?usage: $0 <suite build dir> [example ...]}"); shift
S=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
EXS=${*:-$(cat $S/examples.txt)}
cnt() { sudo -n dmesg 2>/dev/null | grep -cE "$1"; }
d0=$(cnt 'DMA failed'); f0=$(cnt 'Firewall 0 tripped'); p=0; n=0
for ex in $EXS; do
  n=$((n+1)); d=$B/$ex; [ -d $d ] || { echo "$ex: not built"; continue; }
  cd $d; E=$(grep -m1 '^EXECUTABLE' makefile_us_alveo.mk | sed 's/.*=\s*//' | tr -d ' ')
  BD=$(ls -d build_dir.hw.* | head -1); A=$(grep -m1 '^CMD_ARGS' makefile_us_alveo.mk | sed 's/^[^=]*=\s*//')
  A=${A//\$(BUILD_DIR)/$BD}; A=${A//\$(EXECUTABLE)/$E}; A=$(eval echo "$A")
  [ -n "$A" ] || A="$BD/$(ls $BD | grep -m1 '\.xclbin$')"
  unset XRT_INI_PATH
  if [ -f xrt.ini ] && [ "${SUITE_PROFILE:-0}" != 1 ]; then
    grep -vE '^\s*(device_trace|device_counters)\s*=' xrt.ini > xrt.suite.ini; export XRT_INI_PATH=$d/xrt.suite.ini
  fi
  timeout 600 $E $A > run.log 2>&1; rc=$?
  v=$(grep -aoiE "TEST PASSED|TEST FAILED" run.log | tail -1)
  case "$v" in *PASSED*) p=$((p+1));; esac
  printf '%-44s %-12s rc=%s\n' $ex "${v:-no verdict}" $rc
done
echo "SUITE: $p/$n TEST PASSED; DMA failed +$(( $(cnt 'DMA failed') - d0 )), firewall trips +$(( $(cnt 'Firewall 0 tripped') - f0 ))"
