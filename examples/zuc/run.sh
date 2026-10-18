#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [requests] -- ZUC encryption (128-EEA3, "conf") and integrity (128-EIA3, "integ") as a UDP service.
#
# What it shows: requests in UDP frames are answered by the FPGA with no CPU in the data path. The FPGA's own
#   generator (gen_tx) and FLD-lite sit on the two ConnectX-5 ports, joined by the DAC loop:
#   gen_tx -> DAC -> FLD-lite -> ZUC -> FLD-lite -> DAC -> sink_rx, which counts every reply and byte-checks 1 in 4
#   against the ZUC model (zuc/zuc_model.h). First back to back (gap 0) for integ / conf at 8191 and 4096 B and a mix,
#   then a pacing sweep (gen_tx idles between requests).
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo. Sets MTU 9000 on both ports (jumbo
#   frames); HYBRID DDR mode during the run, 'on' after.
# Run:   examples/zuc/run.sh [xclbin] [requests]
#          xclbin    default <repo>/xclbin/zuc.xclbin
#          requests  per run, default 20000
# Prints: a "card:" line, then per run "<label>: posted N txerr N rx N mism N X Gb/s"; "DMA failed during this run: N".
# PASS: every line has posted = rx = the request count, txerr 0, mism 0; DMA failed 0. A run that did not post every
#   request is not a measurement: it prints "*** INVALID RUN" and the rest of its group is skipped.
# Takes: about 30 s.

# 1. the network library (and the card), the xclbin, the request count
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/zuc.xclbin}"); NREQ=${2:-20000}
# 2. checks, kernel reset, HYBRID DDR mode; both ports at MTU 9000 (8191 B requests need jumbo frames)
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 9000 up; done; sleep 5
# r <label> <options>: one e5gen_host run of $NREQ requests; the sed boils its summary down to one line. Options:
#    --jumbo          frames up to 9000 B (16 KB slots); --gring-uram puts gen_tx's rings in the URAM space this frees
#    --zuc            gen_tx sends ZUC requests, sink_rx compares the replies with the model;
#                     --op conf|integ (none: the default mix), --zlen request length in bytes
#    --bf --inline 222  as in fld_echo/run.sh: BlueFlame doorbells, frames up to 222 B inside the send request
#    --gbatch 1       gen_tx posts one request per doorbell (default 16)
#    --sample 2       sink_rx byte-checks 1 reply in 2^2 = 4
r() { local lab=$1; shift; printf '%-30s: ' "$lab"
  o=$(sudo -n env PATH=$PATH timeout 150 $E5/e5gen_host $USER_BDF $X $IB0 $IB1 $CX5_BDF $NREQ "$@" --jumbo --zuc --bf --inline 222 --gbatch 1 --sample 2 --gring-uram 2>&1 \
     | grep -E '^gen_tx|ZUC:|sink_rx|\*\*\*' | sed -E 's/.*posted ([0-9]+).*TX errors ([0-9]+).*/posted \1 txerr \2/; s/sink_rx \(done\): frames ([0-9]+).*MISMATCHES ([0-9]+).*/rx \1 mism \2/; s/ZUC: [0-9.]+ M requests\/s, ([0-9.]+) Gb\/s.*/\1 Gb\/s/' | tr '\n' ' ')
  echo "$o"; echo "$o" | grep -q "posted $NREQ txerr" || { echo '*** INVALID RUN'; return 3; }; }
# 3. back to back: gen_tx with 0 idle cycles between requests
echo '== line rate, gap 0'
for c in '--op integ --zlen 8191' '--op integ --zlen 4096' '--op conf --zlen 8191' '--op conf --zlen 4096' ''; do r "${c:-mix} gap 0" $c --gap 0 || break; done
# 4. paced: --gap N idle kernel cycles after each request; --gnozero: gen_tx does not zero its completion entries
#    after reading them (zero-on-reap off)
echo '== pacing sweep'
for g in 900 1100 1500; do r "conf 4096 gap $g" --op conf --zlen 4096 --gap $g --gnozero || break; done
