#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [variant ...] -- the no-compile alternative to ../run.sh: the same variants, inputs and checks with the
# Python (pyxrt) hosts of this directory instead of the Vitis Libraries C++ hosts (no Vitis_Libraries checkout, no
# OpenCL / Boost / HLS headers, no compiler). Default: every variant whose xclbin is in <repo>/xclbin. Inputs (32 MB
# each, made once in ../data): text, random bytes, zeros. Every FPGA output is checked with the stock tool (lz4 -d /
# gzip -d) against the input; the decompressor is fed stock gzip -6 output and FPGA output. Prints the kernel
# throughput each host reports (host-measured: it includes XRT's dispatch time, so it can read a little lower than
# the C hosts' numbers; the LZ4 host also does its framing in Python between the 32 MB chunks, outside the timing).
set -u
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); REPO=$(cd "$D/../.." && pwd); X=$REPO/xclbin
. "$REPO/common/card_env.sh" >/dev/null || exit 1
source /opt/xilinx/xrt/setup.sh >/dev/null
export INNOVA2_DEV=$USER_BDF
VS=${*:-$(for v in lz4 lz4x4 lz4x8 gzip gzip8 gzip_x4 gunzip gzip_combo; do [ -s $X/$v.xclbin ] && echo $v; done)}
mkdir -p "$D/data"; cd "$D/data"
python3 - <<'PY'
import glob, os
N = 32 << 20
if not os.path.exists('text.bin'):
    t = b''.join(open(f, 'rb').read() for f in sorted(glob.glob('/usr/share/doc/*/copyright')) if os.path.isfile(f))
    open('text.bin', 'wb').write((t * (N // max(len(t), 1) + 1))[:N])
if not os.path.exists('random.bin'): open('random.bin', 'wb').write(os.urandom(N))
if not os.path.exists('zeros.bin'): open('zeros.bin', 'wb').write(bytes(N))
PY
# the decompressor's output buffer is input x mcr: the file's real ratio (+ margin), at least the stock default 20
mcr() { python3 -c "import os,sys; print(max(20, -(-os.path.getsize(sys.argv[1]) // os.path.getsize(sys.argv[2])) + 2))" "$1" "$2"; }
kt() { grep -aE 'KT\(MBps\)' "$1" | tr -s '\t ' ' ' | sed 's/.*KT(MBps) *:\? *//' | head -1; }
ok=0; bad=0; skip=0; res() { if [ "$2" = OK ]; then ok=$((ok+1)); else bad=$((bad+1)); fi; printf '%-11s %-10s %-34s %s MB/s %s\n' "$1" "$3" "$4" "${5:-?}" "$2"; }
for v in $VS; do
  [ -s $X/$v.xclbin ] || { echo "*** no $X/$v.xclbin"; bad=$((bad+1)); continue; }
  for f in text.bin random.bin zeros.bin; do
    case $v in
      lz4*) rm -f $f.lz4; timeout 300 python3 $P/lz4_host.py $X/$v.xclbin $f > $v.$f.log 2>&1
            r=FAIL; lz4 -dc $f.lz4 2>/dev/null | cmp -s - $f && r=OK; res $v $r $f "FPGA compress -> lz4 -d" "$(kt $v.$f.log)" ;;
      gzip|gzip8|gzip_combo)
            rm -f $f.gz; timeout 300 python3 $P/gzip_host.py $X/$v.xclbin $f 1 > $v.$f.log 2>&1
            r=FAIL; gzip -dc $f.gz 2>/dev/null | cmp -s - $f && r=OK; res $v $r $f "FPGA compress -> gzip -d" "$(kt $v.$f.log)" ;;&
      gzip_x4)
            rm -f $f.gz; timeout 300 python3 $P/gzip_host.py $X/$v.xclbin $f 4 > $v.$f.log 2>&1
            r=FAIL; gzip -dc $f.gz 2>/dev/null | cmp -s - $f && r=OK; res $v $r $f "4 CUs compress -> gzip -d" "$(kt $v.$f.log)" ;;
      gunzip|gzip_combo)
            # as run.sh: gzip_decompress_mm returns 0 bytes for streams compressed more than roughly 100-300:1 (stock
            # gzip -6 of zeros is ~1000:1); reported as a SKIP, not a pass
            if [ $f = zeros.bin ]; then printf '%-11s %-10s %-34s %s\n' $v $f "gzip -6 -> FPGA decompress" "SKIP (ratio ~1000:1, see README Results)"; skip=$((skip+1))
            else
            gzip -6 -c $f > $f.cpu.gz; rm -f $f.cpu.gz.orig; timeout 300 python3 $P/gunzip_host.py $X/$v.xclbin $f.cpu.gz $(mcr $f $f.cpu.gz) > $v.$f.d.log 2>&1
            r=FAIL; cmp -s $f.cpu.gz.orig $f && r=OK; res $v $r $f "gzip -6 -> FPGA decompress" "$(kt $v.$f.d.log)"; rm -f $f.cpu.gz.orig
            fi ;;&
      gzip_combo)
            rm -f $f.gz.orig; timeout 300 python3 $P/gunzip_host.py $X/$v.xclbin $f.gz $(mcr $f $f.gz) > $v.$f.dd.log 2>&1
            r=FAIL; cmp -s $f.gz.orig $f && r=OK; res $v $r $f "FPGA compress -> FPGA decompress" "$(kt $v.$f.dd.log)"; rm -f $f.gz.orig ;;
    esac
  done
done
echo "COMPRESSION-PY: $ok OK, $bad FAIL, $skip SKIP; DMA failed in dmesg so far: $(sudo -n dmesg 2>/dev/null | grep -c 'DMA failed')"
[ $bad = 0 ]
