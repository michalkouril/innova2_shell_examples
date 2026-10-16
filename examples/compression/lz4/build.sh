#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# LZ4 compression, 2 engines (Vitis Libraries lz4_compress_mm, URAM dictionaries as published)
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_lz4
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw -); cxo xilLz4Compress $L1 L2/src/lz4_compress_mm.cpp -D PARALLEL_BLOCK=2
link lz4 $REPO/examples/compression/lz4/conn.cfg
