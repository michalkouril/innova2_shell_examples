#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build_hosts.sh <suite build dir> [example ...] -- on the card's host: rebuild every example's host program there,
# with the example's own makefile, against this host's XRT. Hosts compiled against another XRT (the build machine's)
# pass their test and then crash on exit (OpenCL teardown; seen with 2.17-built hosts on XRT 2.27). Needs the
# Vitis_Accel_Examples 'common' directory in <suite build dir>/Vitis_Accel_Examples (build.sh fetches it there).
# XRT >= 2.2x headers need C++17, so the makefile's -std=c++1y / c++14 becomes -std=c++17 (in a copy, makefile_host.mk). A
# host that does not build keeps its previous program.
set -u
B=$(readlink -f "${1:?usage: $0 <suite build dir> [example ...]}"); shift
S=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
[ -d $B/Vitis_Accel_Examples/common/includes ] || { echo "*** $B/Vitis_Accel_Examples/common missing"; exit 2; }
EXS=${*:-$(cat $S/examples.txt)}; ok=0; n=0
for ex in $EXS; do
  n=$((n+1)); d=$B/$ex; [ -d $d ] || { echo "$ex: not built"; continue; }
  cd $d; E=$(grep -m1 '^EXECUTABLE' makefile_us_alveo.mk | sed 's/.*=\s*//' | tr -d ' ')
  sed -E 's/-std=c\+\+(1y|14)/-std=c++17/' makefile_us_alveo.mk > makefile_host.mk
  [ -e $E ] && mv $E $E.prev
  if make -f makefile_host.mk $E TARGET=hw COMMON_REPO=$B/Vitis_Accel_Examples/ \
       PLATFORM=innova2_innova2_xdma_202320_4 > host_build.log 2>&1 && [ -x $E ]; then ok=$((ok+1)); r=OK; rm -f $E.prev
  else r="FAILED ($d/host_build.log)"; [ -e $E.prev ] && mv $E.prev $E; fi
  printf '%-44s %s\n' $ex "$r"
done
echo "SUITE-HOSTS: $ok/$n built"
