#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# NTP server inline in FLD-lite (DESIGN.md): fld_rx -> ntp_resp -> fld_tx answers NTP client requests on UDP port 123
# with timestamps from ntp_tod, a time-of-day counter the host disciplines (ntp_servo.py) through ntp_ctl.
. "$(dirname "$0")/../../common/build_lib.sh"
begin ntp
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
K=$REPO/examples/ntp/kernels
for k in ntp_tod ntp_resp ntp_ctl; do xo $k $K/ntp_kernel.cpp; done
link ntp $REPO/examples/ntp/link.cfg
