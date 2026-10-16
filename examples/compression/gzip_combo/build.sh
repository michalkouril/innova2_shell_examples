#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# GZip compression (8 cores, BRAM) and decompression in one xclbin
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gzip_combo
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw gzip_bram.patch); cxo xilGzipCompBlock $L1 L2/src/gzip_compress_multicore_mm.cpp
L1D=$(l1hw -); cxo xilDecompress $L1D L2/src/gzip_decompress_mm.cpp -D MULTIPLE_BYTES=8 -D GZIP_MODE=1 -D LL_MODEL=true
link gzip_combo $REPO/examples/compression/gzip_combo/conn.cfg
