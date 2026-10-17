#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# lz4_streaming.py <xclbin> [--input FILE | --mb N] [--block-kb 64] [--keep DIR] -- pyxrt host for AMD Vitis Libraries
# data_compression/L2/demos/lz4_streaming (host.cpp + common/libs/compress/lz4OCLHost.cpp, lz4Base.cpp, the -t flow):
# compress the input block by block through xilCompDatamover + xilLz4CompressStream, write AMD's LZ4 frame, check it
# with the stock `lz4 -d`, then decompress the frame through xilDecompDatamover + xilLz4DecompressStream in one launch
# and compare byte for byte. Default input: 32 MB of the host's copyright texts (README: 212 MB/s compress, 1.48 GB/s
# decompress on that input). MB/s = 10^6 bytes of uncompressed data per second of kernel time, as AMD's host.
import argparse, os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', '..', 'common')); sys.path.insert(0, here)
from pyxrt_util import Card, result
import vl_stream as vs

p = argparse.ArgumentParser()
p.add_argument('xclbin'); p.add_argument('--input'); p.add_argument('--mb', type=int, default=32)
p.add_argument('--block-kb', type=int, default=64, choices=sorted(vs.LZ4_BD))
p.add_argument('--keep', help='write the .lz4 here')
a = p.parse_args()
data = vs.load_input(a.input, a.mb << 20)
c = Card(a.xclbin)
blocks, kt_c = vs.compress_blocks(c, c.kernel('xilLz4CompressStream'), c.kernel('xilCompDatamover'), data, a.block_kb * 1024)
frame = vs.lz4_frame_from(blocks, data, a.block_kb)
if a.keep:
    open(os.path.join(a.keep, 'lz4_streaming.lz4'), 'wb').write(frame)
cli = subprocess.run(['lz4', '-d', '-c'], input=frame, capture_output=True)
cli_ok = cli.returncode == 0 and cli.stdout == data
dec, kt_d = vs.decompress_stream(c, c.kernel('xilLz4DecompressStream'), c.kernel('xilDecompDatamover'), frame, len(data))
dec_ok = dec[:len(data)] == data and len(dec) >= len(data)
n = len(data)
print('lz4_streaming: %d B in %d blocks of %d KB -> %d B (CR %.2f); compress %.2f MB/s, stock lz4 -d of it: %s; '
      'decompress %.2f MB/s, %s' % (n, len(blocks), a.block_kb, len(frame), n / len(frame), n / kt_c / 1e6,
                                    'identical' if cli_ok else 'FAILED (rc %d)' % cli.returncode, n / kt_d / 1e6,
                                    'identical' if dec_ok else 'DIFFERENT'))
sys.exit(result('lz4_streaming', cli_ok and dec_ok,
                'compress %.0f MB/s, decompress %.0f MB/s' % (n / kt_c / 1e6, n / kt_d / 1e6)))
