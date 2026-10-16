#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# fetch.sh <dir> [benchmark dir ...] -- AMD Vitis_Libraries v2023.2_update1 (Apache-2.0), sources of every library
# without docs and tests; the data directories only of the benchmarks named (data sets can be large). An argument
# '+<dir>' checks out that directory as well (a tests/ directory a benchmark's Makefile builds from).
set -eu
D=${1:?usage: $0 <dir> [benchmark ...]}; shift; TAG=v2023.2_update1; SHA=fe64280f06dea92eaa95241e43e7e5257f0a3ef0
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null)" != $SHA ]; then
  rm -rf "$D"; git clone -q --filter=blob:none --no-checkout --depth 1 --branch $TAG https://github.com/Xilinx/Vitis_Libraries "$D"
fi
pat=('/*' '!/*/docs/' '!/*/L1/tests/' '!/*/L2/tests/' '!/*/L3/tests/' '!**/data/')
# keep the data directories an earlier fetch checked out ('set' replaces the list), then add these
mapfile -t old < <(git -C "$D" sparse-checkout list 2>/dev/null | grep -E '^/.+/(data|L[123]/tests/[^/]+)/$')
for b in "$@"; do case $b in +*) pat+=("/${b#+}/");; *) pat+=("/$b/data/");; esac; done
mapfile -t pat < <(printf '%s\n' "${pat[@]}" "${old[@]}" | awk 'NF && !seen[$0]++')
git -C "$D" sparse-checkout set --no-cone "${pat[@]}"
git -C "$D" checkout -q $TAG
[ "$(git -C "$D" rev-parse HEAD)" = $SHA ] || { echo "*** $D is not Vitis_Libraries $TAG ($SHA)"; exit 1; }
echo "Vitis_Libraries $TAG at $D"
