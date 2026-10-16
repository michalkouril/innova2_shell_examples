# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# lib.sh -- sourced by compression/*/build.sh after common/build_lib.sh: Vitis Libraries checkout, patched L1 header sets
VL=$BUILD/vitis_libs   # one checkout for all variants; the lock serialises the first clone when they build in parallel
flock "$BUILD/vitis_libs.lock" bash "$REPO/examples/compression/fetch_vitis_libs.sh" "$VL" >/dev/null || exit 1
DC=$VL/data_compression
# l1hw <patch|-> [patch...] : a copy of data_compression/L1/include/hw with the patches applied in order, in the build
# dir; echoes its path
l1hw() {
  local p d=$W/l1hw; for p in "$@"; do d=${d}_${p%.patch}; done
  rm -rf "$d"; cp -a "$DC/L1/include/hw" "$d"
  for p in "$@"; do
    [ "$p" = - ] || patch -s -d "$d" -p1 < "$REPO/examples/compression/patches/$p" || { echo "*** patch $p" >&2; exit 1; }
  done
  echo "$d"
}
# cxo <kernel> <L1 dir> <kernel source (relative to data_compression)> [-D ...]: v++ -c with the library's include set
cxo() {
  local k=$1 l1=$2 src=$3; shift 3
  xo $k "$DC/$src" -I "$VL/security/L1/include" -I "$l1" -I "$DC/L2/include" -I "$DC/L2/src" "$@"
}
