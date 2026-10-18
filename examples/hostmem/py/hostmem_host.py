#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""hostmem_host.py <user BDF> <xclbin> -- the Python (pyxrt) twin of fld_lite/hostmem/hostmem_host.cpp, same tests and
verdict: an RP kernel (hostmem_probe, sp=peer) reads and writes HOST RAM through peer_window windows 2 and 3, pointed
at the 1 MB DMA-coherent buffers of innova2_hostmem.ko (which must be loaded: it is a kernel module, so it is still
built with make). Windows 0/1 (FLD-lite / generator doorbells) are left alone; windows 2/3 are restored at the end.
  1. the kernel writes a pattern into host buffer 0, the host checks every word
  2. the host writes random words into buffer 1, the kernel reads them back and XOR-folds them (checksum)
  3. bandwidth (launch overhead cancels: n large - n small), 4. latency of one dependent read
Run as root. The peer_window registers are written with single 32-bit stores (ctypes), never Python slices.
"""
import ctypes as C
import fcntl
import mmap
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
from pyxrt_util import Card, arg32  # noqa: E402

PW_OFF, MB = 0x100000, 1 << 20
INFO = (2 << 30) | (32 << 16) | (ord('h') << 8) | 1       # _IOR('h', 1, struct hostmem_info {u32 nbuf, size; u64 bus[3]})

if len(sys.argv) < 3:
    sys.exit('usage: %s <user BDF> <xclbin>' % sys.argv[0])
bdf, xclbin = sys.argv[1:3]
try:
    hfd = os.open('/dev/innova2_hostmem', os.O_RDWR)
except OSError as e:
    sys.exit('*** /dev/innova2_hostmem (insmod innova2_hostmem.ko?): %s' % e)
nbuf, size, *bus = struct.unpack('<II3Q', fcntl.ioctl(hfd, INFO, bytes(32)))
hb = [np.frombuffer(mmap.mmap(hfd, MB, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=b * MB), np.uint32)
      for b in range(2)]
print('host buffers: 0 at bus 0x%x, 1 at bus 0x%x (1 MB each, DMA-coherent)' % (bus[0], bus[1]))

c = Card(xclbin, dev=bdf)
k = c.kernel('hostmem_probe')
pbo = c.bo(k, 0, 4096)                                     # locates the PEER aperture
res = c.bo(k, 1, 4096)
print('PEER aperture at 0x%x; window 2 = +2 MB, window 3 = +3 MB' % pbo.address())

bar = mmap.mmap(os.open('/sys/bus/pci/devices/%s/resource0' % bdf, os.O_RDWR | os.O_SYNC), 1 << 25)
pwa = C.addressof(C.c_char.from_buffer(bar)) + PW_OFF


def rd(i):
    return C.c_uint32.from_address(pwa + 4 * i).value


def wr(i, v):
    C.c_uint32.from_address(pwa + 4 * i).value = v & 0xFFFFFFFF


if rd(0) != 0x50454552 or rd(8) < 4:
    sys.exit('*** peer_window v1.1 (4 windows) not found')
ctrl0, s2l, s2h, s3l, s3h = rd(1), rd(0x30 // 4), rd(0x34 // 4), rd(0x38 // 4), rd(0x3C // 4)
wf0, rf0, rr0 = rd(5), rd(6), rd(7)
wr(1, 0)                                                   # bases are written only while disabled
wr(0x30 // 4, bus[0]); wr(0x34 // 4, bus[0] >> 32)
wr(0x38 // 4, bus[1]); wr(0x3C // 4, bus[1] >> 32)
wr(1, 1)
if rd(0x30 // 4) != bus[0] & ~0xFFFFF & 0xFFFFFFFF:
    sys.exit('*** window 2 base did not stick (0x%08x)' % rd(0x30 // 4))
if bus[0] & 0xFFFFF or bus[1] & 0xFFFFF:
    print("NOTE: buffer not 1 MB aligned; the window drops the low 20 bits, offsets compensate")
pofs = pbo.address() & 0x3FFFFF                            # kernel offsets are relative to pbo
a0, a1 = bus[0] & 0xFFFFF, bus[1] & 0xFFFFF
o2, o3 = 2 * MB + a0 - pofs, 3 * MB + a1 - pofs
n2, n3 = (MB - a0) // 32, (MB - a1) // 32                  # the part of each buffer inside its 1 MB window


def run(off, n, mode, p):
    t0 = time.perf_counter()
    c.call(k, pbo, res, off, n, mode, arg32(p))
    return (time.perf_counter() - t0) * 1e6


ok_all = True
# 1. kernel writes host RAM (window 2 -> buffer 0); the host checks every word
hb[0][:] = 0
P = 0xC0FFEE11
run(o2, n2, 0, P)
idx = np.arange(n2 * 8, dtype=np.uint64)
want = (np.uint64(P) ^ ((idx * np.uint64(0x9E3779B9)) & np.uint64(0xFFFFFFFF))).astype(np.uint32)
bad = int(np.count_nonzero(hb[0][a0 // 4:a0 // 4 + n2 * 8] != want))
print('1. kernel WRITE -> host RAM: %u beats, %u wrong words  %s' % (n2, bad, 'FAIL' if bad else 'PASS'))
ok_all &= not bad
# 2. host writes random data (buffer 1); the kernel reads it through window 3 and XOR-folds it
v = np.random.default_rng(42).integers(0, 1 << 32, n3 * 8, dtype=np.uint32)
hb[1][a1 // 4:a1 // 4 + n3 * 8] = v
want = np.bitwise_xor.reduce(v.reshape(-1, 8), axis=0)
run(o3, n3, 1, 0)
rv = res.get(np.uint32, 16)
ok = bool(np.array_equal(rv[:8], want))
print('2. kernel READ  <- host RAM: %u beats, checksum %s (got %08x.. want %08x..)' % (n3, 'PASS' if ok else 'FAIL', rv[0], want[0]))
ok_all &= ok
# 3. bandwidth: launch overhead cancels (n large - n small)
nl, ns = n2, 1024
tw = min(run(o2, nl, 0, P) - run(o2, ns, 0, P) for _ in range(5))
tr = min(run(o3, n3, 1, 0) - run(o3, ns, 1, 0) for _ in range(5))
print('3. bandwidth: kernel->host write %.2f GB/s, host->kernel read %.2f GB/s (%u KB, best of 5)' %
      ((nl - ns) * 32.0 / tw / 1e3, (n3 - ns) * 32.0 / tr / 1e3, nl * 32 // 1024))
# 4. latency of one dependent read from host RAM
m = 20000
tl = min(run(o3, m, 2, 0) - run(o3, 1000, 2, 0) for _ in range(5))
te = min(run(o3, m, 3, 0) - run(o3, 1000, 3, 0) for _ in range(5))
print('4. latency: dependent read from host RAM %.0f ns (loop overhead %.1f ns subtracted)' %
      ((tl - te) / (m - 1000) * 1e3, te / (m - 1000) * 1e3))
print('peer_window counters (this run): writes forwarded %u, reads forwarded %u, refused %u' %
      ((rd(5) - wf0) & 0xFFFFFFFF, (rd(6) - rf0) & 0xFFFFFFFF, (rd(7) - rr0) & 0xFFFFFFFF))
wr(1, 0); wr(0x30 // 4, s2l); wr(0x34 // 4, s2h); wr(0x38 // 4, s3l); wr(0x3C // 4, s3h); wr(1, ctrl0)
print('windows 2/3 restored, CTRL %u\nRESULT %s (tier-1 host memory access)' % (ctrl0, 'PASS' if ok_all else 'FAIL'))
sys.exit(0 if ok_all else 1)
