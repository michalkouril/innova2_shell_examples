#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build_hosts.sh <Vitis_Libraries dir> [benchmark ...] -- on the card's host: build each benchmark's host program there,
# with the benchmark's own Makefile ('make host TARGET=hw'), against this host's XRT. <Vitis_Libraries dir> is a copy
# of $BUILD/vitislibs/Vitis_Libraries from the build machine (build.sh), e.g.
#   rsync -a --exclude '_x_temp.*' --exclude reports --exclude .Xil $BUILD/vitislibs/Vitis_Libraries <host>:<dir>/
# Hosts compiled against another XRT pass and then crash on exit (see examples/suite), and XRT >= 2.2x headers need
# C++17: the Makefiles say -std=c++14, so -std=c++17 is appended (EXTRA_CXXFLAGS; the last -std wins).
# The Makefiles take the HLS headers (ap_int.h ...) from $XILINX_HLS/include and link the HLS floating-point model
# library from $XILINX_HLS/lnx64/tools/fpo_v7_1. A host without Vitis: HLS_INCLUDE = a copy of the HLS include
# directory; the library is then an empty stand-in (a host that really calls it fails to link, loudly).
# Every explicit memory-bank choice in the host sources is first rewritten to bank0 (pin_host.py; the U200/U250 banks
# they name are unused PLRAM / host memory here). Log: <benchmark>/host_build.log. A host that does not build keeps
# its previous program.
# host_patches/<benchmark>.patch (patch -p0, relative to the benchmark directory) is applied first when present: our
# changes to a host -- a missing timing printout, array sizes for AMD's published data sets, the expected result.
set -u
L=$(readlink -f "${1:?usage: $0 <Vitis_Libraries dir> [benchmark ...]}"); shift
S=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
PFM=innova2_innova2_xdma_202320_4
EXS=${*:-$(grep -v '^#' $S/benchmarks.txt | awk 'NF {print $1}')}
FPO=libIp_floating_point_v7_1_bitacc_cmodel
if [ -n "${XILINX_HLS:-}" ] && [ -s $XILINX_HLS/include/ap_int.h ] && [ -e $XILINX_HLS/lnx64/tools/fpo_v7_1/$FPO.so ]; then
  HLS=$XILINX_HLS
else
  I=$(readlink -f "${HLS_INCLUDE:-${XILINX_HLS:-/nonexistent}/include}")
  [ -s "$I/ap_int.h" ] || { echo "*** ap_int.h not found: set HLS_INCLUDE (a copy of Vitis HLS's include directory)"; exit 2; }
  HLS=$L/.hls; mkdir -p $HLS/lnx64/tools/fpo_v7_1; ln -sfn "$I" $HLS/include
  [ -e $HLS/lnx64/tools/fpo_v7_1/$FPO.a ] || ar rc $HLS/lnx64/tools/fpo_v7_1/$FPO.a
fi
ok=0; n=0
for b in $EXS; do
  n=$((n+1)); d=$L/$b; [ -f $d/Makefile ] || { printf '%-48s %s\n' $b "no Makefile"; continue; }
  cd $d; BD=build_dir.hw.$PFM
  E=$BD/$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('host', {}).get('host_exe', 'host.exe'))" description.json)
  hp=""; if [ -f $S/host_patches/$b.patch ]; then   # our host changes (timing printout, array sizes, golden)
    if patch -d $d -p0 -N --dry-run -s < $S/host_patches/$b.patch >/dev/null 2>&1; then
      patch -d $d -p0 -N -s < $S/host_patches/$b.patch && hp=" host_patches applied"
    elif patch -d $d -p0 -R --dry-run -s < $S/host_patches/$b.patch >/dev/null 2>&1; then hp=" host_patches already in"
    else printf '%-48s %s\n' $b "FAILED: host_patches/$b.patch does not apply"; continue; fi
  fi
  np=$(python3 -B $S/pin_host.py $d)   # the hosts name Alveo memory banks: all on bank0 here
  [ -e $E ] && mv $E $E.prev
  if make host TARGET=hw PLATFORM=$PFM XILINX_HLS=$HLS EXTRA_CXXFLAGS=-std=c++17 > host_build.log 2>&1 && [ -x $E ]; then
    ok=$((ok+1)); r=OK; rm -f $E.prev
  else r="FAILED ($d/host_build.log)"; [ -e $E.prev ] && mv $E.prev $E; fi
  printf '%-48s %s\n' $b "$r$([ "${np:-0}" != 0 ] && echo " (bank choices pinned: $np)")$hp"
done
echo "VITISLIBS-HOSTS: $ok/$n built"
