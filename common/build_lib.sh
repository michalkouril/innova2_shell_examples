# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build_lib.sh -- sourced by examples/*/build.sh. Compiles kernels and links one xclbin against the Innova-2 dev platform
# (package innova2-xdma-dev, VBNV innova2_innova2_xdma_202320_4 = shell inn2x r10 "ert4"). The link output loads on the
# card as it is; examples that write to the ConnectX through the PEER window also get innova2_peer_fixup.sh.
#
# Environment:
#   PLATFORM   the .xpfm, or the platform name if the package is installed in /opt/xilinx/platforms
#              (default innova2_innova2_xdma_202320_4)
#   BUILD      output root (default <repo>/build); each example builds in $BUILD/<example>, xclbins go to $BUILD/xclbin
#   VPP_JOBS   v++ synth/impl jobs (default 4)
# Needs Vitis 2023.2 (v++, vivado) and XRT's xclbinutil on PATH.
set -u
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLATFORM=${PLATFORM:-innova2_innova2_xdma_202320_4}
case $PLATFORM in
  *.xpfm) PFM=$PLATFORM ;;
  *) PFM=/opt/xilinx/platforms/$PLATFORM/$PLATFORM.xpfm ;;
esac
[ -s "$PFM" ] || { echo "*** platform $PFM not found (set PLATFORM to the .xpfm)"; exit 2; }
PFMDIR=$(dirname "$PFM")
BUILD=${BUILD:-$REPO/build}
JOBS=${VPP_JOBS:-4}
for t in v++ xclbinutil; do command -v $t >/dev/null || { echo "*** $t not on PATH (source Vitis settings64.sh and XRT setup.sh)"; exit 2; }; done

# begin <example>: cd into $BUILD/<example> (created)
begin() { EX=$1; W=$BUILD/$EX; mkdir -p "$W" "$BUILD/xclbin"; cd "$W"; echo "== $(date +%T) $EX: build in $W, platform $PFM"; }

# xo <kernel> <source> [more sources / v++ options...]: v++ -c into <kernel>.xo. Skipped only when the .xo was built
# from the same arguments and the same sources, including every file in its include directories (the patched header
# copies keep the library's timestamps, so a timestamp check missed patches): the digest is kept in <kernel>.xo.sig.
xo() {
  local k=$1; shift
  local a sig; sig=$( { printf '%s\n' "$PFM" "$@"; for a in "$@"; do
      a=${a#-I}
      if [ -f "$a" ]; then sha256sum "$a"
      elif [ -d "$a" ]; then find "$a" -type f -print0 | sort -z | xargs -0 -r sha256sum; fi
    done; } | sha256sum | cut -c1-64)
  if [ -s $k.xo ] && [ "$(cat $k.xo.sig 2>/dev/null)" = "$sig" ]; then echo "   $k.xo up to date"; return 0; fi
  echo "   v++ -c $k"
  rm -f $k.xo $k.xo.sig
  v++ -c -t hw --platform "$PFM" --temp_dir _xc_$k -k $k -o $k.xo "$@" > v++_c_$k.log 2>&1 \
    || { echo "*** compile $k failed: $W/v++_c_$k.log"; grep -m5 ERROR v++_c_$k.log; exit 1; }
  echo "$sig" > $k.xo.sig
}

# link <name> <link.cfg> [strategy] [want DATA_CLK MHz] [want KERNEL_CLK MHz] -- every *.xo in the build dir;
# gate (common/link_gate.sh); PEER retype when the cfg uses peer; result $BUILD/xclbin/<name>.xclbin
link() {
  local n=$1 cfg=$2 st=${3:-} w0=${4:-} w1=${5:-} rc
  cp "$cfg" link.cfg
  rm -rf _x .ipcache .Xil link.xclbin "$BUILD/xclbin/$n.xclbin"
  echo "   v++ -l $n ${st:+(strategy $st)}"
  v++ -l -t hw --platform "$PFM" --config link.cfg ${st:+--vivado.prop run.impl_1.STRATEGY=$st} \
      --vivado.synth.jobs $JOBS --vivado.impl.jobs $JOBS --temp_dir _x -o link.xclbin $(ls *.xo) > v++_l.log 2>&1; rc=$?
  # (no pipe here: through '| tee' the gate's exit status was lost and a failed link reported BUILD-OK)
  bash "$REPO/common/link_gate.sh" $n $rc v++_l.log _x link.xclbin $w0 $w1 > gate.txt; local g=$?; cat gate.txt
  [ $g = 0 ] || { echo "*** $n: link gate FAILED ($W/v++_l.log)"; grep -m3 "^ERROR" v++_l.log; exit 1; }
  if grep -qiE "^sp=.*:peer$" link.cfg; then
    bash "$PFMDIR/tools/innova2_peer_fixup.sh" link.xclbin "$BUILD/xclbin/$n.xclbin" || exit 1
  else
    cp link.xclbin "$BUILD/xclbin/$n.xclbin" || exit 1
  fi
  xclbinutil --info -i "$BUILD/xclbin/$n.xclbin" 2>/dev/null | grep -E "^ *(UUID|Kernels):" | head -2 | sed 's/^ */   /'
  echo "== $(date +%T) $n BUILD-OK $BUILD/xclbin/$n.xclbin"
}
