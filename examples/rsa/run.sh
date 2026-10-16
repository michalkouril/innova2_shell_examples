#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# run.sh [xclbin] [signatures] -- on the card's host: RSA-2048 signatures on the card (rsa_host.py: pyxrt, every
# signature verified), then the CPU's own rate with all cores (openssl speed rsa2048) for comparison.
D=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$D/../.." && pwd); . "$REPO/common/card_env.sh" >/dev/null || exit 1
X=$(readlink -f "${1:-$REPO/xclbin/rsa.xclbin}"); source /opt/xilinx/xrt/setup.sh >/dev/null
PYTHONPATH=$XILINX_XRT/python${PYTHONPATH:+:$PYTHONPATH} python3 "$D/rsa_host.py" "$X" ${2:-1100} 10 || exit 1
echo "== CPU: openssl speed rsa2048, $(nproc) processes ($(lscpu | sed -n 's/^Model name: *//p'))"
openssl speed -seconds 3 -multi $(nproc) rsa2048 2>/dev/null | grep -iE "rsa.*2048" | tail -1
