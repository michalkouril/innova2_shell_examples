#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# fetch_vitis_libs.sh <dir> -- sparse checkout of AMD Vitis Libraries (Apache-2.0) at the pinned tag v2023.2_update1:
# data_compression, utils, security/L1/include. Verifies the commit. Skips the clone when <dir> already holds it.
set -eu
D=${1:?usage: $0 <dir>}; TAG=v2023.2_update1; SHA=fe64280f06dea92eaa95241e43e7e5257f0a3ef0
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null)" != $SHA ]; then
  rm -rf "$D"; git clone -q --filter=blob:none --no-checkout --depth 1 --branch $TAG https://github.com/Xilinx/Vitis_Libraries "$D"
  git -C "$D" sparse-checkout set --no-cone /data_compression /utils /LICENSE.txt /security/L1/include
  git -C "$D" checkout -q $TAG
fi
[ "$(git -C "$D" rev-parse HEAD)" = $SHA ] || { echo "*** $D is not Vitis_Libraries $TAG ($SHA)"; exit 1; }
echo "Vitis_Libraries $TAG at $D"
