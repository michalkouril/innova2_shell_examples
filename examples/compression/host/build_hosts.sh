#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# build_hosts.sh [vitis_libs dir] -- on the card's host: the Vitis Libraries host programs, as the library's own
# Makefiles build them, into examples/compression/host/bin: xil_lz4 / xil_lz4_4 / xil_lz4_8 (PARALLEL_BLOCK 2/4/8, must
# match the xclbin), xil_gzip and xil_gunzip (both with patches/gzip_host_isize.patch and host_timing.patch), and our gzip_xn_host (several
# compressor CUs at once, for gzip_x4).
# Needs XRT, OpenCL ICD headers/loader (ocl-icd-opencl-dev), Boost headers (libboost-dev), and the HLS arbitrary-precision
# headers (ap_int.h ...): HLS_INCLUDE (default $XILINX_HLS/include, i.e. a Vitis HLS install; on a host without
# Vitis copy that directory over).
set -eu
H=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$H/../../.." && pwd)
VL=${1:-$H/vitis_libs}; bash "$REPO/examples/compression/fetch_vitis_libs.sh" "$VL"
HLS_INCLUDE=${HLS_INCLUDE:-${XILINX_HLS:-/nonexistent}/include}
[ -s "$HLS_INCLUDE/ap_int.h" ] || { echo "*** ap_int.h not found: set HLS_INCLUDE"; exit 1; }
# the hosts build from a patched copy of the library: gzip_host_isize.patch (the gzip footer's ISIZE was stack garbage)
# host_mcr.patch (-mcr above 255 wrapped) and host_timing.patch (the output buffers were copied to the card inside the timed interval, so the printed kernel
# throughput was too low -- for GZip compression by about 4x)
P=$H/vitis_libs_patched; rm -rf "$P"; mkdir -p "$P"; cp -a "$VL/data_compression" "$P/"
for p in gzip_host_isize host_timing host_mcr; do patch -s -d "$P/data_compression" -p1 < "$REPO/examples/compression/patches/$p.patch"; done
mkdir -p "$H/bin"; source /opt/xilinx/xrt/setup.sh >/dev/null
DC=$P/data_compression
for PB in 2 4 8; do OUT=$H/bin/xil_lz4$([ $PB = 2 ] || echo _$PB); echo "   $(basename $OUT)"
  g++ -o $OUT $DC/L2/tests/lz4_compress/src/host.cpp $DC/common/libs/compress/lz4OCLHost.cpp $DC/common/libs/compress/lz4Base.cpp $DC/common/libs/compress/lz4App.cpp $DC/common/libs/compress/compressApp.cpp $DC/common/libs/xcl2/xcl2.cpp $DC/common/libs/cmdparser/cmdlineparser.cpp $DC/common/libs/logger/logger.cpp $DC/common/thirdParty/xxhash/xxhash.c -fmessage-length=0 -I$DC/L2/tests/lz4_compress/src/ -I/opt/xilinx/xrt/include -I$HLS_INCLUDE -std=c++14 -O3 -Wall -Wno-unknown-pragmas -Wno-unused-label   -D PARALLEL_BLOCK=$PB -I $DC/L2/tests/src/ -I $DC/L1/include/hw -I $DC/common/thirdParty/xxhash -I $DC/common/libs/compress/ -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger  -pthread -L/opt/xilinx/xrt/lib  -Wl,--as-needed -lOpenCL -lxrt_coreutil   
done
OUT=$H/bin/xil_gzip; echo "   xil_gzip"
g++ -o $OUT $DC/L2/tests/gzipc_block_mm/src/host.cpp $DC/common/libs/compress/gzipOCLHost.cpp $DC/common/libs/compress/gzipBase.cpp $DC/common/libs/compress/gzipApp.cpp $DC/common/libs/compress/compressApp.cpp $DC/common/libs/xcl2/xcl2.cpp $DC/common/libs/cmdparser/cmdlineparser.cpp $DC/common/libs/logger/logger.cpp $DC/common/thirdParty/zlib-1.2.7/crc32.c $DC/common/thirdParty/zlib-1.2.7/adler32.c -fmessage-length=0 -I$DC/L2/tests/gzipc_block_mm/src/ -I/opt/xilinx/xrt/include -I$HLS_INCLUDE -std=c++14 -O3 -Wall -Wno-unknown-pragmas -Wno-unused-label   -D GZIP_MULTICORE_COMPRESS -D GZIP_MODE -I $DC/L2/tests/src/ -I $DC/L1/include/hw -I $DC/common/libs/compress/ -I $DC/common/thirdParty/zlib-1.2.7 -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger  -pthread -L/opt/xilinx/xrt/lib  -Wl,--as-needed -lOpenCL -lxrt_coreutil   
DC=$P/data_compression; OUT=$H/bin/xil_gunzip; echo "   xil_gunzip"
g++ -o $OUT $DC/L2/tests/gzip_decompress_mm/src/host.cpp $DC/common/libs/compress/gzipOCLHost.cpp $DC/common/libs/compress/gzipBase.cpp $DC/common/libs/compress/gzipApp.cpp $DC/common/libs/compress/compressApp.cpp $DC/common/libs/xcl2/xcl2.cpp $DC/common/libs/cmdparser/cmdlineparser.cpp $DC/common/libs/logger/logger.cpp $DC/common/thirdParty/zlib-1.2.7/crc32.c $DC/common/thirdParty/zlib-1.2.7/adler32.c -fmessage-length=0 -I$DC/L2/tests/gzip_decompress_mm/src/ -I/opt/xilinx/xrt/include -I$HLS_INCLUDE -std=c++14 -O3 -Wall -Wno-unknown-pragmas -Wno-unused-label   -D PARALLEL_BLOCK=1 -D GZIP_MODE=1 -D DECOMPRESS_MM -I $DC/L2/tests/src/ -I $DC/L1/include/hw -I $DC/common/libs/compress/ -I $DC/common/thirdParty/zlib-1.2.7 -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger -I $DC/common/libs/xcl2 -I $DC/common/libs/cmdparser -I $DC/common/libs/logger  -pthread -L/opt/xilinx/xrt/lib  -Wl,--as-needed -lOpenCL -lxrt_coreutil   
OUT=$H/bin/gzip_xn_host; echo "   gzip_xn_host"
g++ -std=c++17 -O2 -o $OUT $H/gzip_xn_host.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib -lxrt_coreutil -pthread
ls "$H/bin"; echo HOSTS-OK
