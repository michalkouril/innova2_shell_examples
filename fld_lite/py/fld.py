# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""fld -- what the three FLD-lite hosts (e5_host, e5gen_host, e5udp_host) share, in Python: the card (pyxrt), the
FPGA-memory views, the peer_window registers, and the ConnectX queues of the FLD-lite port (DevX RQ / RX CQ / TIR /
steering rule + SQ / TX CQ with their rings in a P2P buffer object on the FPGA, frames in FPGA memory, doorbells
through the PEER window), set up exactly as the C hosts do.

Two differences from the C hosts, both forced by the Python binding, neither visible to the kernels:
  * pyxrt passes a scalar kernel argument as 32 bits (the low half of a 64-bit argument; the high half keeps the
    zero of a fresh command), and fld_tx's buf_va is 64-bit: the address the WQEs carry for the TX buffer slots,
    which the C hosts set to the process address of ring + TXB_OFF. So the data MR is registered with
    ibv_reg_mr_iova and an I/O virtual address equal to the BO offset (iova = DATA_OFF for the process address
    ring + DATA_OFF): buf_va becomes TXB_OFF, which fits in 32 bits, and the RQ's data segments carry BO offsets.
    The NIC translates either kind of address to the same FPGA memory. check_arg64() then reads buf_va back from
    fld_tx's control registers, so a non-zero high half is caught (it would also only fail safely: an address
    outside the MR is refused by the NIC with an error CQE, never written).
  * Python has no store fence. Where the C hosts issue _mm_sfence() before the NIC or the FPGA may read what the CPU
    wrote, fence() reads a peer_window register instead: a read from uncached memory (the sysfs BAR mapping) drains
    the write-combining buffers (Intel SDM vol. 3, 11.3.1), which is what the sfence is for there.
"""
import ctypes as C
import mmap
import os
import sys
import time

import numpy as np
import pyxrt

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
sys.path.insert(0, os.path.join(_here, '..', '..', 'common'))
import devx  # noqa: E402
import ifc  # noqa: E402
import verbs  # noqa: E402
from pyxrt_util import arg32  # noqa: E402

PW_OFF = 0x100000
ET_IN, ET_OUT = 0x88B6, 0x88B8
OK = pyxrt.ert_cmd_state.ERT_CMD_STATE_COMPLETED
FROM_DEV = pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_FROM_DEVICE
TO_DEV = pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_TO_DEVICE


def st32(addr, v):
    """One 32-bit store (a C `*(volatile uint32_t *)p = v`)."""
    C.c_uint32.from_address(addr).value = v & 0xFFFFFFFF


def ld32(addr):
    return C.c_uint32.from_address(addr).value


def be32(v):
    return int.from_bytes((v & 0xFFFFFFFF).to_bytes(4, 'big'), 'little')


class Bo:
    """A buffer object, its numpy byte view and its process address."""

    def __init__(self, card, size, group, p2p=False):
        self.size = size
        self.bo = pyxrt.bo(card.dev, size, pyxrt.bo.p2p if p2p else pyxrt.bo.normal, group)
        self.mv = self.bo.map()
        self.mem = np.frombuffer(self.mv, np.uint8)
        self.va = verbs.buf_addr(self.mv)

    def address(self):
        return self.bo.address()

    def zero(self):
        self.mem[:] = 0
        self.bo.sync(TO_DEV, self.size, 0)

    def u32(self, n, off=0):
        self.bo.sync(FROM_DEV, 4 * n, off)
        return self.mem[off:off + 4 * n].view(np.uint32).copy()

    def u64(self, n, off=0):
        self.bo.sync(FROM_DEV, 8 * n, off)
        return self.mem[off:off + 8 * n].view(np.uint64).copy()


class Card:
    def __init__(self, bdf, xclbin):
        self.bdf = bdf
        self.dev = pyxrt.device(bdf)
        self.xclbin_path = xclbin
        self.xb = pyxrt.xclbin(xclbin)
        self.ctx = pyxrt.hw_context(self.dev, self.dev.register_xclbin(self.xb))    # loads the xclbin
        self.k = {}

    def kernel(self, name):
        if name not in self.k:
            self.k[name] = pyxrt.kernel(self.ctx, name)
        return self.k[name]

    def has_kernel(self, name):
        return any(k.get_name() == name for k in self.xb.get_kernels())

    def bo(self, kname, arg, size, p2p=False):
        return Bo(self, size, self.kernel(kname).group_id(arg), p2p)


class PeerWindow:
    """The peer_window registers (user PF BAR0 + 0x100000): 32-bit loads / stores through ctypes."""

    def __init__(self, bdf):
        self.fd = os.open('/sys/bus/pci/devices/%s/resource0' % bdf, os.O_RDWR | os.O_SYNC)
        self.map = mmap.mmap(self.fd, 1 << 25)
        self.bar = verbs.buf_addr(self.map)
        self.base = self.bar + PW_OFF

    def __getitem__(self, i):
        return ld32(self.base + 4 * i)

    def __setitem__(self, i, v):
        st32(self.base + 4 * i, v)

    def fence(self):
        """Drain the CPU's write-combining buffers before the NIC / FPGA looks (see the module docstring)."""
        return ld32(self.base)


def run_args(k, args):
    """A pyxrt.run of kernel k with the arguments {index: Bo | pyxrt.bo | int} (stream arguments left unset)."""
    r = pyxrt.run(k)
    for i, v in args.items():
        r.set_arg(i, v.bo if isinstance(v, Bo) else (arg32(v) if isinstance(v, int) else v))
    return r


def xclbin_section(path, kind):
    """The bytes of the first section of `kind` in an xclbin (axlf: 456-byte header, then 40-byte section headers)."""
    with open(path, 'rb') as f:
        h = f.read(456)
        n = int.from_bytes(h[448:452], 'little')
        for i in range(n):
            f.seek(456 + 40 * i)
            sh = f.read(40)
            if int.from_bytes(sh[0:4], 'little') == kind:
                f.seek(int.from_bytes(sh[24:32], 'little'))
                return f.read(int.from_bytes(sh[32:40], 'little'))
    return None


def check_arg64(card, pw, kname, index, want):
    """Read a started kernel's 64-bit scalar argument back from its control registers (user BAR0 + the CU's base
    address + the argument's offset, both from the xclbin's embedded metadata) -> (ok, value), or (None, None)."""
    import xml.etree.ElementTree as ET
    md = xclbin_section(card.xclbin_path, 2)                 # EMBEDDED_METADATA
    if not md:
        return None, None
    root = ET.fromstring(md.rstrip(b'\0'))
    k = [x for x in root.iter('kernel') if x.get('name') == kname]
    if not k:
        return None, None
    off = [int(x.get('offset'), 16) for x in k[0].iter('arg') if x.get('id') == str(index)]
    base = [int(r.get('base'), 16) for i in k[0].iter('instance') for r in i.iter('addrRemap')
            if r.get('port') == 'S_AXI_CONTROL' and r.get('base')]
    if not off or not base or base[0] + off[0] + 8 > 1 << 25:
        return None, None
    a = pw.bar + base[0] + off[0]
    v = ld32(a) | (ld32(a + 4) << 32)
    return v == want, v


def wait(r, ms):
    return r.wait(ms) == OK


def dn(ok):
    return 'done' if ok else 'NOT done'


class FldPort:
    """The FLD-lite side (one ConnectX port): rings in `ring` (a P2P Bo at bank0 offset 0) at the offsets of `lay`,
    the data MR over RX slots .. TX slots, the queues, PEER window 0 -> this port's UAR. lay keys: UMEM_OFF UMEM_LEN
    RQ_OFF RQ_DBR RCQ_OFF RCQ_DBR SQ_OFF SQ_DBR TCQ_OFF TCQ_DBR LOG_RQ LOG_RCQ LOG_SQ LOG_TCQ LOG_TXS DATA_OFF TXB_OFF
    STRIDE STOP_OFFS (list of stop-word offsets to clear). rule: dict(ethertype=..) or dict(udp=True, ...)."""

    def __init__(self, V, L, ring, dev_name, cx5_bdf, pw, pbo_addr, lay, rule, ctx=None):
        self.lay, self.ring = lay, ring
        um = lay['UMEM_OFF']
        self.ctx = ctx or V.open_dev(dev_name, True)
        if not self.ctx:
            raise SystemExit('cannot open %s' % dev_name)
        self.pd = V.ibv_alloc_pd(self.ctx)
        mr_len = lay['TXB_OFF'] + (lay['STRIDE'] << lay['LOG_TXS']) - lay['DATA_OFF']
        self.iova0 = 0                                # iova of ring + x = x: BO offsets (module docstring)
        self.dmr = V.reg_mr(self.pd, ring.va + lay['DATA_OFF'], mr_len, iova=self.iova0 + lay['DATA_OFF'])
        self.umem = V.umem_reg(self.ctx, ring.va + um, lay['UMEM_LEN'])
        if not self.dmr or not self.umem:
            raise SystemExit('registration failed: %s (is innova2_peermem loaded?)' % os.strerror(V.errno()))
        self.pdn = V.pdn(self.pd)
        self.uar = V.alloc_uar(self.ctx)
        m = ring.mem
        m[um:um + lay['UMEM_LEN']] = 0
        for so in lay['STOP_OFFS']:
            m[so:so + 64] = 0
        m[um + lay['RCQ_OFF'] + 63:um + lay['RCQ_OFF'] + (64 << lay['LOG_RCQ']):64] = 0xF1
        m[um + lay['TCQ_OFF'] + 63:um + lay['TCQ_OFF'] + (64 << lay['LOG_TCQ']):64] = 0xF1
        for i in range(1 << lay['LOG_RQ']):          # RQ data segments, in iova addresses
            verbs.set_data_seg(m, um + lay['RQ_OFF'] + i * 16, lay['STRIDE'], self.dmr.lkey,
                               self.iova0 + lay['DATA_OFF'] + i * lay['STRIDE'])
        pw.fence()
        self.rx = devx.rx_create(V, L, self.ctx, self.pdn, self.uar.page_id, self.umem.umem_id, lay['RCQ_OFF'], lay['RCQ_DBR'],
                                 lay['RQ_OFF'], lay['RQ_DBR'], lay['LOG_RCQ'], lay['LOG_RQ'], **rule)
        st32(ring.va + um + lay['RQ_DBR'], be32(1 << lay['LOG_RQ']))
        self.tcq = devx.cq_create(V, L, self.ctx, self.uar.page_id, self.umem.umem_id, lay['TCQ_OFF'], lay['TCQ_DBR'], lay['LOG_TCQ'])
        try:
            mi = devx.query_min_inline(V, L, self.ctx)
        except devx.DevxError:
            mi = 1
        self.sq = devx.sq_create(V, L, self.ctx, self.pdn, self.tcq['cqn'], self.uar.page_id, self.umem.umem_id, lay['SQ_OFF'],
                                 self.umem.umem_id, lay['SQ_DBR'], lay['LOG_SQ'], mi)
        pw.fence()
        self.uar_bus = verbs.bar_start(cx5_bdf, 0) + self.uar.page_id * 4096 + (self.uar.reg_addr - self.uar.base_addr)
        self.db_off = ((self.uar_bus & 0xFFFFF) - (pbo_addr & 0xFFFFF)) & 0xFFFFFFFF
        self.buf_va = self.iova0 + lay['TXB_OFF']     # fld_tx buf_va: the iova of ring + TXB_OFF (< 2^32)


def peer_window0(pw, uar_bus):
    """PEER window 0 -> uar_bus (bases are written only while disabled); returns the writes-forwarded counter."""
    if pw[0] != 0x50454552:
        raise SystemExit('*** peer_window not found -- is the E3 shell running?')
    pw[1] = 0
    pw[2] = uar_bus & ~0xFFFFF
    pw[3] = uar_bus >> 32
    w0 = pw[5]
    pw[1] = 1
    return w0


def st3(v, name):
    if not v:
        print('  %-40s (no samples)' % name)
        return
    v = sorted(v)
    print('  %-40s mean %6.2f us  p50 %6.2f  p99 %6.2f' % (name, sum(v) / len(v), v[len(v) // 2], v[len(v) * 99 // 100]))


def cqe_ts(mem, off):
    """The 63-bit timestamp of the 64-byte CQE at mem[off] (bytes 48..55, big-endian)."""
    return int.from_bytes(mem[off + 48:off + 56].tobytes(), 'big') & 0x7fffffffffffffff


def env_ifc():
    return verbs.Verbs(), ifc.Ifc()


def sleep_s(s):
    time.sleep(s)
