#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# check_resize.sh ref <Vitis_Libraries dir> <out dir>   (build machine: Vitis, a built tree from build.sh)
# check_resize.sh run <Vitis_Libraries dir> <out dir>   (card host, after build_hosts.sh; <out dir> copied from ref)
# codec/L2/demos/resize at the sizes AMD published (7680x4320 -> 512x512 / 1920x1080 / 3840x2160), with the output
# checked: its own host never compares the result (nerror is never set). 'ref' makes a deterministic 8-bit test image
# (in8k.raw) and the expected outputs with software emulation of the same kernel source (AMD's C++ model); 'run' runs
# the card and compares byte for byte, printing the kernel time.
set -u
M=${1:?ref|run}; L=$(readlink -f "${2:?Vitis_Libraries dir}"); O=$(mkdir -p "${3:?out dir}" && readlink -f "$3")
S=$(cd "$(dirname "$0")" && pwd); D=$L/codec/L2/demos/resize; PFM=innova2_innova2_xdma_202320_4
SIZES="512x512 1920x1080 3840x2160"
if [ $M = ref ]; then
  [ -s $O/in8k.raw ] || python3 -I -c "
import numpy as np
y, x = np.mgrid[0:4320, 0:7680].astype(np.float32)
img = 128 + 60*np.sin(x/37.0)*np.cos(y/53.0) + 40*(x/7680) - 30*(y/4320)
img += np.random.default_rng(20261016).normal(0, 8, (4320, 7680))
np.clip(img, 0, 255).astype(np.uint8).tofile('$O/in8k.raw')"
  # a copy of the codec library (the Makefile takes the host sources from the library, and the build tree keeps its hw
  # build), banks pinned as build_hosts.sh does; sw_emu needs the multiarch dirs (Vitis 2023.2 g++)
  T=$O/swemu/Vitis_Libraries; rm -rf $O/swemu; mkdir -p $T
  rsync -a --exclude '_x_temp.*' --exclude 'build_dir.*' --exclude reports --exclude innova2_reports $L/codec $T/
  [ -d $L/utils ] && rsync -a $L/utils $T/
  T=$T/codec/L2/demos/resize; python3 -B $S/pin_host.py $T >/dev/null; for c in $T/*.cfg; do python3 -B $S/pin_cfg.py $c >/dev/null; done
  cd $T; export CPATH=/usr/include/x86_64-linux-gnu LIBRARY_PATH=/usr/lib/x86_64-linux-gnu VPP_LDFLAGS="--config $T/conn_u50.cfg"
  make xclbin host emconfig TARGET=sw_emu PLATFORM=$PLATFORM > $O/swemu_build.log 2>&1 ||
    { echo "*** sw_emu build failed ($O/swemu_build.log)"; exit 1; }
  BD=$(ls -d $T/build_dir.sw_emu.*); export XCL_EMULATION_MODE=sw_emu XCL_BINDIR=$BD
  for s in $SIZES; do cp $O/in8k.raw $O/ref_$s.raw
    (cd $BD && ./host.exe -xclbin kernel_resize.xclbin -i $O/ref_$s.raw -srcw 7680 -srch 4320 -dstw ${s%x*} -dsth ${s#*x}) \
      > $O/ref_$s.log 2>&1 || { echo "*** sw_emu run $s failed ($O/ref_$s.log)"; exit 1; }
    rm -f $O/ref_$s.raw; echo "reference $s: $(stat -c%s $O/ref_${s}_resized.raw) bytes"; done
  exit 0
fi
source /opt/xilinx/xrt/setup.sh >/dev/null; BD=$D/build_dir.hw.$PFM; bad=0
for s in $SIZES; do
  [ -s $O/ref_${s}_resized.raw ] || { echo "*** no reference $O/ref_${s}_resized.raw (run 'ref')"; exit 2; }
  cp $O/in8k.raw $O/hw_$s.raw; rm -f $O/hw_${s}_resized.raw
  timeout 120 $BD/host.exe -xclbin $BD/kernel_resize.xclbin -i $O/hw_$s.raw -srcw 7680 -srch 4320 -dstw ${s%x*} \
    -dsth ${s#*x} > $O/hw_$s.log 2>&1; rc=$?; rm -f $O/hw_$s.raw
  if [ $rc = 0 ] && cmp -s $O/hw_${s}_resized.raw $O/ref_${s}_resized.raw; then v=PASS; else v=FAIL; bad=1; fi
  echo "resize 7680x4320 -> $s: $v rc=$rc $(grep -aoE 'Time in kernel: [0-9.]+ ?ms' $O/hw_$s.log)"
done
exit $bad
