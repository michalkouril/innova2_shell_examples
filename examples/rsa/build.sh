#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# rsa: RSA-2048 signing offload, step 1 -- one rsa_modexp compute unit with RSA_NE engines (default 9): each engine
# runs 4 exponentiations (2 signatures, both CRT halves) at a time, 4 limbs per step, 140 DSPs. The dynamic region has
# 1,632 DSP sites; 11 engines routed only at 195 MHz (congestion), so 9 -- which v++ still clocks below 300: 213 MHz
# in the 2026-10-16 build (7,526 signatures/s, README Results).
. "$(dirname "$0")/../../common/build_lib.sh"
begin rsa
K=$REPO/examples/rsa/kernel
xo rsa_modexp $K/rsa_modexp.cpp -DRSA_NE=${RSA_NE:-9} -DRSA_U=${RSA_U:-4} --hls.pre_tcl $K/rsa_hls.tcl
link rsa $REPO/examples/rsa/link.cfg
