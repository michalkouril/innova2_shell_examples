#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [frames] -- FLD-lite as an echo at full speed, with no CPU in the data path.
#
# What it shows: the FPGA drives the ConnectX-5 itself. On port 1 the FPGA's traffic generator (gen_tx) sends frames;
#   the DAC loop carries them to port 0, where FLD-lite (fld_rx -> afu_loop -> fld_tx) echoes every frame back; on
#   port 1 sink_rx counts them and byte-checks 1 frame in 16. Frame sizes 64 B - 1500 B, back to back (gap 0). Then the
#   idle echo latency: one 64 B frame at a time from the host (e5_host), the ConnectX-5's timestamps splitting each
#   round trip into host, wire and FPGA parts.
# Needs: what common/net_lib.sh lists -- the card running inn2x r10 with P2P on, `bash fld_lite/host_setup.sh` run once,
#   the DAC cable between the two ConnectX-5 ports, passwordless sudo. Sets MTU 1500 on both ports; HYBRID DDR mode
#   during the run, 'on' after.
# Run:   examples/fld_echo/run.sh [xclbin] [frames]
#          xclbin  default <repo>/xclbin/fld_echo.xclbin
#          frames  frames per size, default 200000
#        USER_BDF=<FPGA user PF> picks the card on a host with several.
# Prints: a "card:" line; one line per frame size with gen_tx (posted, TX errors), sink_rx (frames, RX error CQEs,
#   checked, MISMATCHES) and "RATE through FLD-lite: N M frames/s"; the latency table (round trip, host, wire out,
#   FPGA echo, wire back; mean / p50 / p99 in us); "DMA failed during this run: N".
# PASS: TX errors 0, RX error CQEs 0, MISMATCHES 0 on every line, DMA failed 0. At 64 - 512 B sink_rx may count fewer
#   frames than were posted: frames lost when FLD-lite falls behind at that size, not errors; the rate is the result.
# Takes: about 15 s.

# 1. the network library (and the card), the xclbin, the frame count
D=$(cd "$(dirname "$0")" && pwd); . "$D/../../common/net_lib.sh"
X=$(readlink -f "${1:-$REPO/xclbin/fld_echo.xclbin}"); NF=${2:-200000}
# 2. checks, kernel reset, HYBRID DDR mode (common/net_lib.sh); both ports up at MTU 1500; 5 s for the links
preflight; for i in $NET0 $NET1; do sudo -n ip link set $i mtu 1500 up; done; sleep 5
# 3. the echo sweep: one e5gen_host run per frame length. It loads the xclbin, sets up both ports' queues, runs
#    gen_tx -> FLD-lite -> sink_rx and prints its counters; the grep keeps the summary lines, joined into one line.
#      --len L --gap 0  frame length; gen_tx waits 0 idle cycles between batches (as fast as it can)
#      --bf             BlueFlame: the send request (WQE) is written straight into the ConnectX-5's doorbell page, so
#                       the NIC need not fetch it over PCIe (without it: an 8-byte doorbell, then the NIC reads the WQE)
#      --inline 222     frames up to 222 B (the most a WQE holds) are copied into the WQE: no separate read of the frame
#      --inline-adapt   ... but only lone frames on an idle link
#      --rxk 4          fld_rx takes up to 4 receive completions (CQEs) per poll
#      --sample 4       sink_rx byte-checks 1 frame in 2^4 = 16 (checking every frame makes sink_rx the bottleneck)
#      timeout 150      ends a run that hangs
for len in 64 256 512 1024 1500; do
  printf 'len %-5s: ' $len
  sudo -n env PATH=$PATH timeout 150 $E5/e5gen_host $USER_BDF $X $IB0 $IB1 $CX5_BDF $NF --len $len --gap 0 --bf --inline 222 --inline-adapt --rxk 4 --sample 4 2>&1 \
    | grep -E '^gen_tx|sink_rx|frames/s|Gb/s|\*\*\*' | tr '\n' ' '; echo
done
# 4. idle echo latency: e5_host sends 2000 64 B frames one at a time from the host on port 1 (--lat; ConnectX-5
#    timestamps split each round trip). --pre 1026 = 2 | 4 << 8: fld_rx reads the first 2 beats of the frame together
#    with the CQE, and takes up to 4 CQEs per poll (as --rxk 4 above). The tail keeps the latency table.
echo '== idle echo latency (64 B)'
sudo -n env PATH=$PATH timeout 150 $E5/e5_host $USER_BDF $X $IB0 $IB1 $CX5_BDF 2000 --lat --len 64 --bf --inline 222 --pre 1026 2>&1 | grep -E 'lat|median|p99|\*\*\*' | tail -6
