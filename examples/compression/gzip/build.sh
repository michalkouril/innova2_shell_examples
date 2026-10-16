#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# GZip compression, 2 cores (gzip_compress_multicore_mm, URAM as published)
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gzip
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw -); cxo xilGzipCompBlock $L1 L2/src/gzip_compress_multicore_mm.cpp -D NUM_CORES=2
link gzip $REPO/examples/compression/gzip/conn.cfg
