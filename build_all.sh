#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build_all.sh [-j N] [example ...] -- build every example (or the ones named) with N links at a time (default 2; a
# link takes 30-90 minutes and 8-12 GB). Logs: $BUILD/<example>.out; summary at the end. Environment: see
# common/build_lib.sh (PLATFORM, BUILD, VPP_JOBS).
set -u
REPO=$(cd "$(dirname "$0")" && pwd); J=2
[ "${1:-}" = -j ] && { J=$2; shift 2; }
ALL=$(cd "$REPO/examples" && find . -name build.sh -printf "%h\n" | sed "s#^\./##" | grep -v "^suite" | sort)
EXS=${*:-$ALL}
export BUILD=${BUILD:-$REPO/build}; mkdir -p "$BUILD"
echo "$EXS" | tr ' ' '\n' | xargs -P "$J" -I{} bash -c 'o="$BUILD/$(echo {} | tr / _).out"; bash "$0/examples/{}/build.sh" > "$o" 2>&1; echo "{} rc=$?"' "$REPO"
for e in $EXS; do printf '%-14s %s\n' "$e" "$(grep -hE 'BUILD-OK|\*\*\*' "$BUILD/$(echo $e | tr / _).out" | tail -1)"; done
