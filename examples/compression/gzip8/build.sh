#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# GZip compression, 8 cores: LZ77 dictionaries and the distributor/merger FIFOs moved to BRAM (patches/gzip_bram.patch)
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gzip8
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw gzip_bram.patch); cxo xilGzipCompBlock $L1 L2/src/gzip_compress_multicore_mm.cpp
link gzip8 $REPO/examples/compression/gzip8/conn.cfg
