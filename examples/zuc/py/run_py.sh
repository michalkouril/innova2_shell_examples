#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run_py.sh [xclbin] [requests] -- the no-compile alternative to ../run.sh, same runs and verdicts:
# gen_tx -> DAC -> FLD-lite -> ZUC -> FLD-lite -> DAC -> sink_rx, every reply checked against the ZUC model, set up by
# fld_lite/py/e5gen_host.py (expected replies from fld_lite/py/zuc_model.py). Line rate (gap 0) for INTEG/CONF at
# 8191 and 4096 bytes and a mix, then a pacing sweep. A run that did not post every request is reported as INVALID.
P=$(cd "$(dirname "$0")" && pwd); D=$(cd "$P/.." && pwd); . "$D/../../common/net_lib_py.sh"
X=$(readlink -f "${1:-$REPO/xclbin/zuc.xclbin}"); NREQ=${2:-20000}
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 9000 up; done; sleep 5
r() { local lab=$1; shift; printf '%-30s: ' "$lab"
  o=$(sudo -n env $PYENV timeout 300 python3 $E5PY/e5gen_host.py $USER_BDF $X $IB0 $IB1 $CX5_BDF $NREQ "$@" --jumbo --zuc --bf --inline 222 --gbatch 1 --sample 2 --gring-uram 2>&1 \
     | grep -E '^gen_tx|ZUC:|sink_rx|\*\*\*|Error' | sed -E 's/.*posted ([0-9]+).*TX errors ([0-9]+).*/posted \1 txerr \2/; s/sink_rx \(done\): frames ([0-9]+).*MISMATCHES ([0-9]+).*/rx \1 mism \2/; s/ZUC: [0-9.]+ M requests\/s, ([0-9.]+) Gb\/s.*/\1 Gb\/s/' | tr '\n' ' ')
  echo "$o"; echo "$o" | grep -q "posted $NREQ txerr" || { echo '*** INVALID RUN'; return 3; }; }
echo '== line rate, gap 0'
for c in '--op integ --zlen 8191' '--op integ --zlen 4096' '--op conf --zlen 8191' '--op conf --zlen 4096' ''; do r "${c:-mix} gap 0" $c --gap 0 || break; done
echo '== pacing sweep'
for g in 900 1100 1500; do r "conf 4096 gap $g" --op conf --zlen 4096 --gap $g --gnozero || break; done
