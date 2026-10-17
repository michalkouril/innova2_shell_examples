#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# snappy_streaming.py <xclbin> [--input FILE | --mb N] [--block-kb K] [--decompress-mb M] -- pyxrt host for AMD
# Vitis Libraries data_compression/L2/demos/snappy_streaming (host.cpp + common/libs/compress/snappyOCLHost.cpp,
# snappyBase.cpp; SNAPPY_STREAM build): compress block by block through xilCompDatamover + xilSnappyCompressStream into
# AMD's framed stream (CRC fields 0), decode that stream with an independent pure-Python snappy decoder, and
# decompress through xilDecompDatamover + xilSnappyDecompressStream (one launch) with a byte-for-byte compare.
# The stock kernel's decompressor hangs once the output passes 64 KB (amd_ip_bugs V18), so by default the FPGA
# decompression here runs on a 64 KB input only; snappy_streaming_fix.py runs the fixed kernel at full size.
# Defaults = the README row: 4 MB of the host's copyright texts in 4 MB blocks (285 MB/s compress). With blocks of
# 2 MB or more AMD's compressor writes a wrong uncompressed-length preamble (the size mod 2^21: 4 MB -> 0); its own
# decompressor ignores it, standard decoders do not. The data itself decodes correctly; that is reported, and is a
# failure only with --strict.
import argparse, os, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', '..', 'common')); sys.path.insert(0, here)
from pyxrt_util import Card, result
import vl_stream as vs


def main(name, mb, block_kb, dec_mb, pyenc_mb):
    p = argparse.ArgumentParser()
    p.add_argument('xclbin'); p.add_argument('--input'); p.add_argument('--mb', type=float, default=mb)
    p.add_argument('--block-kb', type=int, default=block_kb, choices=(64, 256, 1024, 4096))
    p.add_argument('--decompress-mb', type=float, default=dec_mb,
                   help='FPGA round trip on this much of the input (0: all of it; the stock kernel hangs above 64 KB)')
    p.add_argument('--pyenc-mb', type=float, default=pyenc_mb, help='also decompress a Python-encoded stream of this size')
    p.add_argument('--strict', action='store_true', help='fail on a wrong snappy length preamble too (AMD: blocks >= 2 MB)')
    a = p.parse_args()
    data = vs.load_input(a.input, int(a.mb * (1 << 20)))
    c = Card(a.xclbin)
    ck, cdm = c.kernel('xilSnappyCompressStream'), c.kernel('xilCompDatamover')
    dk, ddm = c.kernel('xilSnappyDecompressStream'), c.kernel('xilDecompDatamover')
    blocks, kt_c = vs.compress_blocks(c, ck, cdm, data, a.block_kb * 1024)
    frame = vs.snappy_frame_from(blocks, data)
    info = {}
    try:
        py_ok = vs.snappy_frame_decode(frame, info) == data
    except ValueError as e:
        print('python snappy decoder: %s' % e); py_ok = False
    if info.get('bad_preamble'):
        print('%s: AMD compressor defect: %d of %d blocks carry a wrong uncompressed-length preamble (a varint of the '
              'size mod 2^21; standard snappy decoders reject such a stream)' % (name, info['bad_preamble'], len(blocks)))
        py_ok = py_ok and not a.strict
    n = len(data); ok = py_ok
    print('%s: %d B in %d blocks of %d KB -> %d B (CR %.2f); compress %.2f MB/s; Python decode of the FPGA stream: %s'
          % (name, n, len(blocks), a.block_kb, len(frame), n / len(frame), n / kt_c / 1e6, 'identical' if py_ok else 'FAILED'))
    detail = 'compress %.0f MB/s' % (n / kt_c / 1e6)
    # FPGA round trip (on the first decompress-mb of the input when that is smaller)
    rt = data if a.decompress_mb == 0 else data[:int(a.decompress_mb * (1 << 20))]
    if rt is not data:
        blk2, _ = vs.compress_blocks(c, ck, cdm, rt, 64 * 1024)
        f2 = vs.snappy_frame_from(blk2, rt)
    else:
        f2 = frame
    dec, kt_d = vs.decompress_stream(c, dk, ddm, f2, len(rt))
    rt_ok = dec[:len(rt)] == rt
    print('%s: FPGA decompress of the FPGA stream, %d B: %.2f MB/s, %s' % (name, len(rt), len(rt) / kt_d / 1e6,
                                                                           'identical' if rt_ok else 'DIFFERENT'))
    ok = ok and rt_ok; detail += ', decompress %.0f MB/s (%d B)' % (len(rt) / kt_d / 1e6, len(rt))
    if a.pyenc_mb > 0:
        src = data[:int(a.pyenc_mb * (1 << 20))]
        f3 = vs.snappy_frame_encode(src)
        dec3, kt3 = vs.decompress_stream(c, dk, ddm, f3, len(src))
        e_ok = dec3[:len(src)] == src
        print('%s: FPGA decompress of a Python-encoded stream, %d B: %.2f MB/s, %s'
              % (name, len(src), len(src) / kt3 / 1e6, 'identical' if e_ok else 'DIFFERENT'))
        ok = ok and e_ok
    return result(name, ok, detail)


if __name__ == '__main__':
    sys.exit(main('snappy_streaming', 4, 4096, 0.0625, 0))
