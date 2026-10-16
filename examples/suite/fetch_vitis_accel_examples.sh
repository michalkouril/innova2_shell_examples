#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# fetch_vitis_accel_examples.sh <dir> -- AMD Vitis_Accel_Examples (MIT) at the commit this suite was validated with.
set -eu
D=${1:?usage: $0 <dir>}; SHA=d1a35c3999a7a5ae32a450b8800bc329edbb261d
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null)" != $SHA ]; then
  rm -rf "$D"; git clone -q https://github.com/Xilinx/Vitis_Accel_Examples "$D"; git -C "$D" checkout -q $SHA
fi
[ "$(git -C "$D" rev-parse HEAD)" = $SHA ] || { echo "*** $D is not Vitis_Accel_Examples $SHA"; exit 1; }
echo "Vitis_Accel_Examples $SHA at $D"
