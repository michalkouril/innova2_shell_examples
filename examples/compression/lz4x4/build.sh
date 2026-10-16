#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# LZ4 compression, 4 engines (24 of the 32 free URAMs hold the dictionaries)
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_lz4x4
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw -); cxo xilLz4Compress $L1 L2/src/lz4_compress_mm.cpp -D PARALLEL_BLOCK=4
link lz4x4 $REPO/examples/compression/lz4x4/conn.cfg
