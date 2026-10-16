#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [requests] -- on the card's host. The FPGA's own generator (gen_tx) and FLD-lite sit on the two
# ConnectX ports, joined by the DAC loop: gen_tx -> DAC -> FLD-lite -> ZUC -> FLD-lite -> DAC -> sink_rx, which checks
# every reply against the ZUC model. Line rate (gap 0) for
# INTEG/CONF at 8191 and 4096 bytes and a mix, then a pacing sweep. Each line: requests posted, TX errors, replies,
# mismatches, Gb/s. A run that did not post every request is reported as INVALID.
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/zuc.xclbin}"); NREQ=${2:-20000}
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 9000 up; done; sleep 5
r() { local lab=$1; shift; printf '%-30s: ' "$lab"
  o=$(sudo -n env PATH=$PATH timeout 150 $E5/e5gen_host $USER_BDF $X $IB0 $IB1 $CX5_BDF $NREQ "$@" --jumbo --zuc --bf --inline 222 --gbatch 1 --sample 2 --gring-uram 2>&1 \
     | grep -E '^gen_tx|ZUC:|sink_rx|\*\*\*' | sed -E 's/.*posted ([0-9]+).*TX errors ([0-9]+).*/posted \1 txerr \2/; s/sink_rx \(done\): frames ([0-9]+).*MISMATCHES ([0-9]+).*/rx \1 mism \2/; s/ZUC: [0-9.]+ M requests\/s, ([0-9.]+) Gb\/s.*/\1 Gb\/s/' | tr '\n' ' ')
  echo "$o"; echo "$o" | grep -q "posted $NREQ txerr" || { echo '*** INVALID RUN'; return 3; }; }
echo '== line rate, gap 0'
for c in '--op integ --zlen 8191' '--op integ --zlen 4096' '--op conf --zlen 8191' '--op conf --zlen 4096' ''; do r "${c:-mix} gap 0" $c --gap 0 || break; done
echo '== pacing sweep'
for g in 900 1100 1500; do r "conf 4096 gap $g" --op conf --zlen 4096 --gap $g --gnozero || break; done
