#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# fetch_fns.sh <dir> -- ETH Zurich's fpga-network-stack (github.com/fpgasystems/fpga-network-stack, BSD-3-Clause) at
# the commit the iperf client was validated with; build_client.sh compiles its hls/iperf_udp client unchanged.
set -eu
D=$1; SHA=75c32bae73f5c336b5c46c9de35ae0cb9cb7cc1c
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null)" != $SHA ]; then
  rm -rf "$D"; git clone -q https://github.com/fpgasystems/fpga-network-stack "$D"; git -C "$D" checkout -q $SHA
fi
[ "$(git -C "$D" rev-parse HEAD)" = $SHA ] || { echo "*** $D is not fpga-network-stack $SHA"; exit 1; }
