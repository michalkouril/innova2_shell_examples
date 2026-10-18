#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh <Vitis_Libraries dir> [benchmark ...] -- AMD Vitis Libraries benchmarks on the card, with their own C++ hosts.
#
# What it shows: each benchmark of benchmarks.txt (or the ones named) runs with the command its description.json gives
#   (launch[0].cmd_args), or the args= of its benchmarks.txt line (the rest of the line; e.g. AMD's benchmark input
#   size); in both, BUILD = its build_dir.hw.<platform>, PROJECT = the benchmark directory, LIB_DIR = the library,
#   e.g. .../security.
# Needs: the card running inn2x r10; the benchmarks built in <Vitis_Libraries dir> (build.sh, build_hosts.sh);
#   python3; sudo only to count kernel messages. The hosts open the first Xilinx device, so on a host with several
#   cards make sure that is the Innova-2. No cable, no DDR mode change.
# Run:   examples/vitislibs/run.sh <Vitis_Libraries dir> [benchmark ...]
#   RUN_TIMEOUT   seconds per benchmark (default 600)
#   VL_PROFILE=1  keep the profiling / tracing options of the benchmark's xrt.ini (left out by default)
# Prints: per benchmark "<benchmark> PASS|FAIL rc=N <seconds> s" and up to 6 timing / throughput lines of its output
#   (all of it in <benchmark>/run.log), or why it did not run; then "VITISLIBS: P/N PASS; DMA failed +N, firewall
#   trips +N" (kernel messages added during the run).
# PASS: per benchmark exit code 0 and no FAIL / ERROR line in the output; overall P = N, DMA failed +0, firewall
#   trips +0.
# Takes: depends on the selection; each benchmark is limited to RUN_TIMEOUT.

set -u
# 1. arguments, XRT, the platform, the benchmark list; the kernel-message counts at the start
L=$(readlink -f "${1:?usage: $0 <Vitis_Libraries dir> [benchmark ...]}"); shift
S=$(cd "$(dirname "$0")" && pwd); source /opt/xilinx/xrt/setup.sh >/dev/null
PFM=innova2_innova2_xdma_202320_4
EXS=${*:-$(grep -v '^#' $S/benchmarks.txt | awk 'NF {print $1}')}
cnt() { sudo -n dmesg 2>/dev/null | grep -cE "$1"; }
d0=$(cnt 'DMA failed'); f0=$(cnt 'Firewall 0 tripped'); p=0; n=0
# 2. each benchmark that has an xclbin
for b in $EXS; do
  n=$((n+1)); d=$L/$b; BD=$d/build_dir.hw.$PFM
  ls $BD/*.xclbin >/dev/null 2>&1 || { printf '%-48s %s\n' $b "no xclbin"; continue; }
  # its host program and arguments: description.json, or the args= of its benchmarks.txt line
  read -r E A < <(python3 - "$d" "$BD" "$L/${b%%/*}" "$b" "$S/benchmarks.txt" <<'PY'
import json, sys
d, bd, lib, b, lst = sys.argv[1:]
j = json.load(open(d + '/description.json'))
a = j['launch'][0].get('cmd_args', '')
for ln in open(lst):
    if ln.split()[:1] == [b] and 'args=' in ln:
        a = ln.split('args=', 1)[1]
for k, v in (('BUILD', bd), ('PROJECT', d), ('LIB_DIR', lib)):
    a = a.replace(k, v)
print(bd + '/' + j.get('host', {}).get('host_exe', 'host.exe'), ' '.join(a.split()))
PY
  )
  # pin_host.py --check must accept the xclbin, and the host program must be built
  m=$(python3 -B $S/pin_host.py --check $(ls $BD/*.xclbin | head -1)) || { printf '%-48s %s\n' $b "FAIL: $m"; continue; }
  [ -x "$E" ] || { printf '%-48s %s\n' $b "no host program (build_hosts.sh)"; continue; }
  # the benchmark's xrt.ini without profiling / tracing (VL_PROFILE=1 keeps them): device_trace / device_counter make
  # XRT 2.27 hosts crash on exit (zstd_decompress: SIGSEGV after a correct run), and tracing slows the runs measured
  unset XRT_INI_PATH
  if [ -f $d/xrt.ini ] && [ "${VL_PROFILE:-0}" != 1 ]; then
    grep -vE '^\s*(profile|timeline_trace|device_profile|device_trace|device_counters?|opencl_trace|data_transfer_trace|native_xrt_trace|stall_trace|power_profile|opencl_summary|opencl_device_counter)\s*=' \
      $d/xrt.ini > $d/xrt.innova2.ini; export XRT_INI_PATH=$d/xrt.innova2.ini
  fi
  # run it in its directory, timed; PASS = exit code 0 and no FAIL / ERROR line in run.log
  cd $d; t0=$(date +%s%N)
  timeout ${RUN_TIMEOUT:-600} $E $A > run.log 2>&1; rc=$?
  s=$(( ($(date +%s%N) - t0) / 1000000 )); s=$((s / 1000)).$(printf %03d $((s % 1000)) | cut -c1)
  if [ $rc = 0 ] && ! { grep -aqiE 'test fail|\bFAIL(ED)?\b' run.log || grep -aqE '^ *\[?ERROR\b|ERROR:' run.log; }; then v=PASS; p=$((p+1)); else v=FAIL; fi
  printf '%-48s %-4s rc=%-3s %5.1f s\n' $b $v $rc $s
  grep -aiE 'execution time|throughput|[GM]B/s|latency' run.log | head -6 | sed 's/^/      /'
done
# 3. the total and the kernel messages this run added
echo "VITISLIBS: $p/$n PASS; DMA failed +$(( $(cnt 'DMA failed') - d0 )), firewall trips +$(( $(cnt 'Firewall 0 tripped') - f0 ))"
