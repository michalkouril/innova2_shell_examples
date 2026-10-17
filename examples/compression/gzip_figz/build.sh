#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# gzip_figz: the xclbin for tools/figz -- the 8-core GZip compressor (BRAM dictionaries, patches/gzip_bram.patch) and
# four GZip decompressors, so figz decompresses up to four of its members at once (each decompressor is ~12k LUTs).
# The compressor carries LZ77 history across its 32 KB blocks (patches/gzip_hist.patch): each block is preceded by the
# last 8 KB of the one before, which its core reads but does not emit (from a separate 8 KB FIFO per core), and
# matches are extended over offsets up to 32 KB: on the HP Z440 test host text went from 4.55x to 5.06x (gzip -1: 4.90x) and figz
# compression from 1.23 to 0.97 GB/s. The congestion strategy keeps the data clock at 205 MHz (157 MHz without).
. "$(dirname "$0")/../../../common/build_lib.sh"
begin compression_gzip_figz
. "$REPO/examples/compression/lib.sh"
L1=$(l1hw gzip_bram.patch gzip_hist.patch)
cxo xilGzipCompBlock $L1 L2/src/gzip_compress_multicore_mm.cpp -D GZIP_HIST_BYTES=8192 -D GZIP_BOOST_WIN=32768
L1D=$(l1hw -); cxo xilDecompress $L1D L2/src/gzip_decompress_mm.cpp -D MULTIPLE_BYTES=8 -D GZIP_MODE=1 -D LL_MODEL=true
link gzip_figz $REPO/examples/compression/gzip_figz/conn.cfg Congestion_SpreadLogic_high
