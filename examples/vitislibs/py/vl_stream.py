# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
"""vl_stream -- shared by the pyxrt hosts of the Vitis Libraries streaming compression demos (lz4_streaming,
snappy_streaming[_fix], zstd_decompress[_fix]). Ported from AMD Vitis Libraries data_compression/common/libs/compress
(lz4OCLHost.cpp, lz4Base.cpp, snappyOCLHost.cpp, snappyBase.cpp, zstdOCLHost.cpp): the same kernel sequence per
block / per file, the same frame formats; plus test inputs and independent checks (lz4 / zstd CLIs, a pure-Python
snappy decoder and xxHash32)."""
import glob
import os
import struct
import time

import numpy as np

from pyxrt_util import start_args, wait_ok

STUCK = 'did not complete within %d s (stuck CU: load another xclbin to reset it)'


def text_input(nbytes):
    """The README's text input: this host's /usr/share/doc/*/copyright files, concatenated and repeated to nbytes
    (the same generator as examples/compression/run.sh)."""
    t = b''.join(open(f, 'rb').read() for f in sorted(glob.glob('/usr/share/doc/*/copyright')) if os.path.isfile(f))
    if not t:
        t = bytes(np.random.default_rng(1).integers(32, 127, 1 << 20, np.uint8))
    return (t * (nbytes // len(t) + 1))[:nbytes]


def load_input(path, nbytes):
    return open(path, 'rb').read() if path else text_input(nbytes)


# ---- xxHash32 (LZ4 frame header and content checksums) --------------------------------------------------------------
_P1, _P2, _P3, _P4, _P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
_M = 0xFFFFFFFF


def _rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & _M


def xxh32(data, seed=0):
    n = len(data); i = 0
    if n >= 16:
        v = [(seed + _P1 + _P2) & _M, (seed + _P2) & _M, seed & _M, (seed - _P1) & _M]
        # 16-byte stripes, vectorised: each lane is a running product, so do it lane by lane over uint32 words
        w = np.frombuffer(data, np.uint32, (n // 16) * 4).reshape(-1, 4)
        for j in range(4):
            acc = v[j]
            for x in w[:, j].tolist():
                acc = (_rotl((acc + x * _P2) & _M, 13) * _P1) & _M
            v[j] = acc
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & _M
        i = (n // 16) * 16
    else:
        h = (seed + _P5) & _M
    h = (h + n) & _M
    while i + 4 <= n:
        h = (_rotl((h + struct.unpack_from('<I', data, i)[0] * _P3) & _M, 17) * _P4) & _M; i += 4
    while i < n:
        h = (_rotl((h + data[i] * _P5) & _M, 11) * _P1) & _M; i += 1
    h ^= h >> 15; h = (h * _P2) & _M; h ^= h >> 13; h = (h * _P3) & _M; h ^= h >> 16
    return h


# ---- block-wise compression through a datamover + stream kernel (lz4OCLHost / snappyOCLHost::compressEngineStreamSeq)
def compress_blocks(c, comp_k, dm_k, data, block, timeout_s=60):
    """Per block of `block` bytes: xilCompDatamover(in, out, compressed_size, size) feeding the compress stream kernel
    (its scalar arg 2 = size). Returns ([(compressed block bytes or None if not smaller, original size)], kernel s)."""
    n = len(data); nblk = (n + block - 1) // block
    inb = c.bo(dm_k, 0, nblk * block).put(np.frombuffer(data, np.uint8))
    outb = c.bo(dm_k, 1, block); szb = c.bo(dm_k, 2, 64)
    import pyxrt
    blocks, kt = [], 0.0
    for b in range(nblk):
        size = min(block, n - b * block)
        sub = pyxrt.bo(inb.bo, size, b * block)
        t0 = time.perf_counter()
        rk = start_args(comp_k, {2: size})
        rd = start_args(dm_k, {0: sub, 1: outb, 2: szb, 3: size})
        ok = wait_ok(rd, timeout_s) and wait_ok(rk, timeout_s)
        kt += time.perf_counter() - t0
        if not ok:
            raise RuntimeError('compress block %d ' % b + STUCK % timeout_s)
        cs = int(szb.get(np.uint32, 1)[0])
        blocks.append((outb.get(np.uint8, cs).tobytes() if cs < size else None, size))
    return blocks, kt


def decompress_stream(c, dec_k, dm_k, comp, out_size, timeout_s=60):
    """xilDecompDatamover(in, out, input_size, outputSize) + the decompress stream kernel (scalar arg 3 = input size),
    one launch over the whole compressed file (lz4OCLHost / snappyOCLHost::decompressEngineStreamSeq). Returns
    (decompressed bytes, kernel s)."""
    inb = c.bo(dm_k, 0, len(comp)).put(np.frombuffer(comp, np.uint8))
    outb = c.bo(dm_k, 1, out_size + 4096).zero(); szb = c.bo(dm_k, 3, 64).zero()
    t0 = time.perf_counter()
    rk = start_args(dec_k, {3: len(comp)})
    rd = start_args(dm_k, {0: inb, 1: outb, 2: len(comp), 3: szb})
    ok = wait_ok(rd, timeout_s) and wait_ok(rk, timeout_s)
    kt = time.perf_counter() - t0
    if not ok:
        raise RuntimeError('decompress ' + STUCK % timeout_s)
    n = int(szb.get(np.uint32, 1)[0])
    return outb.get(np.uint8, min(n, out_size + 4096)).tobytes(), kt


# ---- LZ4 frame as lz4Base writes it (FLG 0x64: independent blocks, content checksum; BD for the block size) ----------
LZ4_BD = {64: 0x40, 256: 0x50, 1024: 0x60, 4096: 0x70}
LZ4_NCOMP = {64: 1, 256: 4, 1024: 0x10, 4096: 0x40}     # byte 2 of a stored (uncompressed) full block's length word


def lz4_frame_from(blocks, data, block_kb):
    """The LZ4 frame of lz4OCLHost::xilCompress: header, blocks (compressed, or stored when not smaller), end mark,
    content checksum (xxHash32 of the input)."""
    flg, bd = 0x64, LZ4_BD[block_kb]
    out = bytearray(b'\x04\x22\x4d\x18') + bytes([flg, bd, (xxh32(bytes([flg, bd])) >> 8) & 0xFF])
    off = 0
    for comp, size in blocks:
        if comp is not None:
            out += struct.pack('<I', len(comp)) + comp
        else:
            hdr = bytes([0, 0, LZ4_NCOMP[block_kb], 0x80]) if size == block_kb * 1024 else \
                bytes([size & 0xFF, (size >> 8) & 0xFF, (size >> 16) & 0xFF, 0x80])
            out += hdr + data[off:off + size]
        off += size
    out += b'\0\0\0\0' + struct.pack('<I', xxh32(data))
    return bytes(out)


# ---- snappy framing as snappyOCLHost writes it (CRCs left 0) -----------------------------------------------------------
SNAPPY_HDR = b'\xff\x06\x00\x00sNaPpY'


def snappy_frame_from(blocks, data):
    out = bytearray(SNAPPY_HDR); off = 0
    for comp, size in blocks:
        if comp is not None:
            out += b'\x00' + struct.pack('<I', len(comp) + 4)[:3] + b'\0\0\0\0' + comp
        else:
            out += b'\x01' + struct.pack('<I', size + 4)[:3] + b'\0\0\0\0' + data[off:off + size]
        off += size
    return bytes(out)


def snappy_raw_decode(b, info=None):
    """Decode one raw snappy block (independent pure-Python reference). The length preamble is checked against the
    decoded size; with info (a dict) a mismatch is counted in info['bad_preamble'] instead of raised (AMD's compressor
    writes a wrong preamble for blocks of 2 MB and more)."""
    i, n, shift = 0, 0, 0
    while True:
        x = b[i]; i += 1; n |= (x & 0x7F) << shift; shift += 7
        if x < 0x80:
            break
    out = bytearray(); L = len(b)
    while i < L:
        tag = b[i]; i += 1; t = tag & 3
        if t == 0:
            ln = tag >> 2
            if ln >= 60:
                k = ln - 59; ln = int.from_bytes(b[i:i + k], 'little'); i += k
            ln += 1; out += b[i:i + ln]; i += ln
            continue
        if t == 1:
            ln = 4 + ((tag >> 2) & 7); off = ((tag >> 5) << 8) | b[i]; i += 1
        elif t == 2:
            ln = (tag >> 2) + 1; off = b[i] | (b[i + 1] << 8); i += 2
        else:
            ln = (tag >> 2) + 1; off = int.from_bytes(b[i:i + 4], 'little'); i += 4
        if off == 0 or off > len(out):
            raise ValueError('bad snappy copy offset %d at %d' % (off, i))
        s = len(out) - off
        if off >= ln:
            out += out[s:s + ln]
        else:
            for j in range(ln):
                out.append(out[s + j])
    if len(out) != n:
        if info is None:
            raise ValueError('snappy block decoded to %d bytes, header says %d' % (len(out), n))
        info['bad_preamble'] = info.get('bad_preamble', 0) + 1
    return bytes(out)


def snappy_frame_decode(f, info=None):
    """Decode AMD's snappy framed stream (stream identifier, compressed / uncompressed chunks; CRC fields ignored:
    AMD's host writes 0)."""
    if not f.startswith(SNAPPY_HDR):
        raise ValueError('no snappy stream identifier')
    i, out = len(SNAPPY_HDR), bytearray()
    while i < len(f):
        typ = f[i]; ln = int.from_bytes(f[i + 1:i + 4], 'little'); body = f[i + 4:i + 4 + ln]; i += 4 + ln
        if typ == 0:
            out += snappy_raw_decode(body[4:], info)
        elif typ == 1:
            out += body[4:]
        elif typ == 0xFF or 0x80 <= typ <= 0xFE:
            continue
        else:
            raise ValueError('unknown snappy chunk type %d' % typ)
    return bytes(out)


def snappy_raw_encode(data):
    """A simple greedy snappy compressor (hash of 4 bytes, 64 KB fragments) to make decompression-only test streams
    that did not come from the FPGA."""
    out = bytearray(); n = len(data); v = n
    while True:
        out.append((v & 0x7F) | (0x80 if v > 0x7F else 0)); v >>= 7
        if not v:
            break

    def lit(s, e):
        while s < e:
            ln = min(e - s, 1 << 16); m = ln - 1
            if m < 60:
                out.append(m << 2)
            elif m < 256:
                out.extend((60 << 2, m))
            else:
                out.extend((61 << 2, m & 0xFF, m >> 8))
            out.extend(data[s:s + ln]); s += ln

    table, i, last = {}, 0, 0
    while i + 4 <= n:
        key = data[i:i + 4]; j = table.get(key); table[key] = i
        if j is not None and i - j < 65536:
            ln = 4
            while i + ln < n and ln < 64 and data[j + ln] == data[i + ln]:
                ln += 1
            lit(last, i); off = i - j
            out.extend((((ln - 1) << 2) | 2, off & 0xFF, off >> 8))
            i += ln; last = i
        else:
            i += 1
    lit(last, n)
    return bytes(out)


def snappy_frame_encode(data, chunk=65536):
    out = bytearray(SNAPPY_HDR)
    for o in range(0, len(data), chunk):
        c = snappy_raw_encode(data[o:o + chunk])
        out += b'\x00' + struct.pack('<I', len(c) + 4)[:3] + b'\0\0\0\0' + c
    return bytes(out)
