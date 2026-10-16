#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# GZip decompression (gzip_decompress_mm: 8 bytes/cycle literal decoder, GZip mode)
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gunzip
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw -); cxo xilDecompress $L1 L2/src/gzip_decompress_mm.cpp -D MULTIPLE_BYTES=8 -D GZIP_MODE=1 -D LL_MODEL=true
link gunzip $REPO/examples/compression/gunzip/conn.cfg
