#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# LZ4 compression, 8 engines: dictionaries moved from URAM to BRAM (patches/lz4_dict_bram.patch) so 8 fit
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_lz4x8
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw lz4_dict_bram.patch); cxo xilLz4Compress $L1 L2/src/lz4_compress_mm.cpp -D PARALLEL_BLOCK=8
link lz4x8 $REPO/examples/compression/lz4x8/conn.cfg
