#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# GZip compression with four 2-core compute units, LZ77 dictionaries and FIFOs in BRAM (patches/gzip_bram.patch): one
# 8-core CU is capped by a shared per-CU stage at ~2 bytes/clock, so four small CUs run side by side instead. The host
# (host/gzip_xn_host) gives each CU a quarter of the input and concatenates the four gzip members.
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gzip_x4
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw gzip_bram.patch); cxo xilGzipCompBlock $L1 L2/src/gzip_compress_multicore_mm.cpp -D NUM_CORES=2
link gzip_x4 $REPO/examples/compression/gzip_x4/conn.cfg
