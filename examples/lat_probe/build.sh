#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# lat_probe: one kernel that times dependent reads / writes on its memory port, for a per-memory latency map.
. "$(dirname "$0")/../../common/build_lib.sh"
begin lat_probe
xo lat_probe $REPO/fld_lite/lat/lat_probe.cpp
link lat_probe $REPO/examples/lat_probe/link.cfg
