# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

"""ntp_lib -- shared by ntp_servo.py and ntp_client.py: the ntp_ctl kernel (pyxrt) and an NTP client socket with
kernel software timestamps (SO_TIMESTAMPING: T1 from the transmit error queue, T4 from the receive path), so both
ends of the client's measurement are taken where the packet enters / leaves the host's network stack."""
import os
import socket
import struct
import time

NTP_EPOCH = 2208988800                       # 1900 -> 1970
OP_RATE, OP_STEP, OP_SNAP, OP_SET, OP_COUNT = 1, 2, 3, 4, 5
RF = dict(leap=0, stratum=1, prec=2, rdelay=3, rdisp=4, refid=5, ref_lo=6, ref_hi=7, cal_rx=8, cal_tx=9)
COUNTERS = ["frames", "replies", "drop_len", "drop_hdr", "drop_mode", "res_last", "res_max", "tod_full", "tod_cmds"]


def ns_to_ntp(ns):
    s, f = divmod(ns, 1_000_000_000)
    return ((s + NTP_EPOCH) << 32) | ((f << 32) // 1_000_000_000)


def ntp_to_ns(t):
    return ((t >> 32) - NTP_EPOCH) * 1_000_000_000 + (((t & 0xffffffff) * 1_000_000_000) >> 32)


def ntp_sub_ns(a, b):
    """a - b for two NTP timestamps, in ns (signed, era-safe for |a - b| < 68 years)"""
    d = (a - b) & 0xffffffffffffffff
    if d >= 1 << 63:
        d -= 1 << 64
    return (d * 1_000_000_000) >> 32 if d >= 0 else -(((-d) * 1_000_000_000) >> 32)


class Ctl:
    """ntp_ctl: one command per kernel run; out[] in a 64-byte bank0 BO"""

    def __init__(self, xclbin, bdf=None):
        import pyxrt
        self.x = pyxrt
        self.dev = pyxrt.device(bdf if bdf else 0)
        self.ctx = pyxrt.hw_context(self.dev, self.dev.register_xclbin(pyxrt.xclbin(xclbin)))
        self.k = pyxrt.kernel(self.ctx, "ntp_ctl")
        self.bo = pyxrt.bo(self.dev, 64, pyxrt.bo.normal, self.k.group_id(3))

    def run(self, op, a=0, b=0, nout=0):
        s32 = lambda v: (v & 0xffffffff) - ((v & 0x80000000) << 1)   # pyxrt rejects ints >= 2^31 for 32-bit args
        r = self.k(op, s32(a), s32(b), self.bo)
        r.wait()
        if not nout:
            return []
        self.bo.sync(self.x.xclBOSyncDirection.XCL_BO_SYNC_BO_FROM_DEVICE, 4 * nout, 0)
        return list(struct.unpack("<%dI" % nout, bytes(self.bo.read(4 * nout, 0))))

    def rate(self, r):
        self.run(OP_RATE, r & 0xffffffff, r >> 32)

    def step_ntp(self, d):
        """tod += d (2^-32 s units, signed)"""
        d &= 0xffffffffffffffff
        self.run(OP_STEP, d & 0xffffffff, d >> 32)

    def snap(self):
        lo, hi = self.run(OP_SNAP, nout=2)
        return hi << 32 | lo

    def set(self, field, v):
        self.run(OP_SET, RF[field], v)

    def counters(self):
        return dict(zip(COUNTERS, self.run(OP_COUNT, nout=len(COUNTERS))))


SO_TIMESTAMPING = 37
SOF_TX_SW, SOF_RX_SW, SOF_SW, SOF_OPT_TSONLY = 1 << 1, 1 << 3, 1 << 4, 1 << 11


def _ts(anc):
    for lvl, typ, data in anc:
        if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPING and len(data) >= 16:
            s, n = struct.unpack("qq", data[:16])
            if s or n:
                return s * 1_000_000_000 + n
    return None


class Client:
    """NTP client socket; query() -> dict with T1..T4 (ns since 1970, host clock) and the reply's fields"""

    def __init__(self, server, port=123, timeout=0.2):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPING, SOF_TX_SW | SOF_RX_SW | SOF_SW | SOF_OPT_TSONLY)
        self.s.connect((server, port))
        self.s.settimeout(timeout)
        self.seq = int.from_bytes(os.urandom(4), "big")

    def _tx_ts(self):
        for _ in range(200):
            try:
                _, anc, _, _ = self.s.recvmsg(64, 1024, socket.MSG_ERRQUEUE | socket.MSG_DONTWAIT)
                t = _ts(anc)
                if t:
                    return t
            except BlockingIOError:
                time.sleep(0.00002)
        return None

    def send_raw(self, pkt):
        self.s.send(pkt)
        self._tx_ts()

    def recv_raw(self):
        try:
            return self.s.recv(2048)
        except socket.timeout:
            return None

    def query(self, vn=4, mode=3):
        self.seq = (self.seq + 1) & 0xffffffff
        xmt = ns_to_ntp(time.time_ns()) & ~0xffffffff | self.seq        # unique transmit timestamp (low word = seq)
        pkt = struct.pack("!BBbb II 4s QQQQ", (0 << 6) | (vn << 3) | mode, 0, 6, -20, 0, 0, b"\0" * 4, 0, 0, 0, xmt)
        t1a = time.time_ns()
        self.s.send(pkt)
        t1 = self._tx_ts() or t1a
        while True:
            try:
                data, anc, _, _ = self.s.recvmsg(2048, 1024)
            except socket.timeout:
                return None
            t4 = _ts(anc) or time.time_ns()
            if len(data) < 48:
                continue
            f = struct.unpack("!BBbb II 4s QQQQ", data[:48])
            if f[8] != xmt:                                            # origin: a late reply to an earlier query
                continue
            lvm = f[0]
            r = dict(li=lvm >> 6, vn=(lvm >> 3) & 7, mode=lvm & 7, stratum=f[1], poll=f[2], prec=f[3], rdelay=f[4],
                     rdisp=f[5], refid=f[6], ref=f[7], org=f[8], rec=f[9], xmt=f[10], sent_vn=vn,
                     t1=t1, t4=t4, sw_t1=t1 != t1a)
            r["t2"], r["t3"] = ntp_to_ns(f[9]), ntp_to_ns(f[10])
            r["offset"] = ((r["t2"] - t1) + (r["t3"] - t4)) / 2         # server - host
            r["delay"] = (t4 - t1) - (r["t3"] - r["t2"])
            r["res"] = r["t3"] - r["t2"]
            return r
