#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# memcached (Xilinx HLx memcached pipeline, binary protocol over UDP) on FLD-lite + udp_lite: fld_rx -> udp_rx_lite ->
# mcd -> udp_tx_lite -> fld_tx; hash table and value store in DDR bank0 (mcd_mem), counters in mcd_dbg.
. "$(dirname "$0")/../../common/build_lib.sh"
begin memcached
F=$REPO/fld_lite/e5
for k in fld_rx fld_tx fld_reap e5_ticker2; do xo $k $F/fld_lite.cpp --hls.pre_tcl $F/fld_lite_hls.tcl; done
for k in udp_rx_lite udp_tx_lite; do xo $k $F/udp_lite.cpp; done
M=$REPO/examples/memcached/kernels; P=$M/pipeline
S="$M/mcd_kernel.cpp $P/hashTable/cc.cpp $P/hashTable/compare.cpp $P/hashTable/hash.cpp $P/hashTable/hashTable.cpp $P/hashTable/memRead.cpp $P/hashTable/memWrite.cpp $P/requestParser/requestParser.cpp $P/responseFormatter/binResponse.cpp $P/valueStore/valueStore.cpp $P/valueStore/flashValueStore.cpp $P/globals.cpp $P/merger.cpp $P/splitter.cpp"
for k in mcd mcd_mem mcd_dbg; do xo $k $S -I$P --hls.pre_tcl $M/mcd_hls.tcl; done
link memcached $REPO/examples/memcached/link.cfg Performance_ExplorePostRoutePhysOpt
