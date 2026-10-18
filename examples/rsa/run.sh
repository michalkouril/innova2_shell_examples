#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [signatures] -- RSA-2048 signing on the card, compared with the host's CPU.
#
# What it shows: rsa_host.py (pyxrt, no build step) makes 4 RSA-2048 keys and the requested number of messages, runs
#   the two CRT halves of every signature on the card's rsa_modexp engines (10 rounds), recombines and verifies every
#   signature on the host, and reports signatures/s and the latency of one signature; then `openssl speed rsa2048` with
#   one process per core gives the CPU's rate for comparison.
# Needs: the card running inn2x r10; python3, numpy, XRT's pyxrt; openssl. No root, no cable, no DDR mode change.
# Run:   examples/rsa/run.sh [xclbin] [signatures]      (default <repo>/xclbin/rsa.xclbin, 1100 signatures)
# Prints: "RSA-2048 sign on the card: ... signatures/s ...", then "== CPU: openssl speed rsa2048, N processes (<CPU>)"
#   and openssl's rsa 2048 line (sign/s, verify/s).
# PASS: rsa_host.py exits 0 (every signature verified); otherwise the script stops there with its exit code.
# Takes: under a minute.

# 1. the card (its summary line hidden), the xclbin, XRT
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" >/dev/null || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/rsa.xclbin}"); source /opt/xilinx/xrt/setup.sh >/dev/null
# 2. the card: the signatures, 10 rounds (pyxrt is in $XILINX_XRT/python)
PYTHONPATH=$XILINX_XRT/python${PYTHONPATH:+:$PYTHONPATH} python3 "$D/rsa_host.py" "$X" ${2:-1100} 10 || exit 1
# 3. the CPU: openssl speed for 3 s, one process per core
echo "== CPU: openssl speed rsa2048, $(nproc) processes ($(lscpu | sed -n 's/^Model name: *//p'))"
openssl speed -seconds 3 -multi $(nproc) rsa2048 2>/dev/null | grep -iE "rsa.*2048" | tail -1
