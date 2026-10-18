#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""e5_host.py -- the no-compile (Python) counterpart of ../e5/e5_host.cpp: FLD-lite (fld_rx -> afu_loop -> fld_tx)
on port 0, set up exactly as e5_host does (fld_lite/py/fld.py), and the host drives echo traffic from port 1 through
the loopback cable. Same frames, same checks (every echo byte-compared), same RESULT line.

One difference: e5_host sends and receives on port 1 with a raw-packet verbs QP and ConnectX timestamps; this host
uses a Linux AF_PACKET socket on port 1's netdev (the kernel's network stack, no verbs data path to re-create in
Python). So port 1 has no NIC timestamps: the rate and the round trip are on the host clock and include the kernel
stack, and --lat cannot split the wire times out. The FPGA's own share (port 0 RX CQE -> TX CQE, from the CQE
timestamps in FPGA memory) is measured exactly as the C host measures it.

usage (root): e5_host.py <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF of port0> [frames] [options]
  --lat  ping-pong, one frame in flight (forces --sig 1)   --burst  unpaced   --len L (60..1500) | --mix
  --bf --inline N --inline-adapt --sig K --stamp --rxddr --pre N --gap N --maxb N   (as e5_host)
"""
import os
import select
import socket
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devx  # noqa: E402
import fld  # noqa: E402
from fld import ET_IN, ET_OUT, st32  # noqa: E402

LENS = (60, 64, 100, 128, 200, 222, 223, 256, 511, 1000, 1500, 777)


def main(argv):
    if len(argv) < 6:
        print('usage: %s <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF> [frames] [options]' % argv[0], file=sys.stderr)
        return 2
    n = int(argv[6]) if len(argv) > 6 and argv[6][0] != '-' else 1000
    BUF, STRIDE = 8 << 20, 2048
    LOG_RQ, LOG_RCQ, LOG_SQ, LOG_TCQ, LOG_TXS = 8, 11, 10, 11, 10
    DATA_OFF, UMEM_OFF, UMEM_LEN, TXB_OFF, STOP_OFF = 0x80000, 0x0, 0x60000, 0x400000, 0x380000
    RQ_OFF, RQ_DBR, RCQ_DBR, SQ_DBR, TCQ_DBR = 0x0, 0x4000, 0x4040, 0x4080, 0x40C0
    SQ_OFF, RCQ_OFF, TCQ_OFF = 0x10000, 0x20000, 0x40000
    FRAME, MIX = 1024, False
    lat = burst = False
    bf, inl, sig, stamp, spec, gap, maxb = 0, 0, 16, 0, 2, 64, 16
    a = argv[6:]
    for i, o in enumerate(a):
        v = a[i + 1] if i + 1 < len(a) else None
        if o == '--lat': lat = True
        if o == '--burst': burst = True
        if o == '--bf': bf = 1
        if o == '--mix': MIX = True
        if o == '--inline' and v: inl = int(v)
        if o == '--inline-adapt': inl |= 1 << 16
        if o == '--len' and v: FRAME = int(v)
        if o == '--sig' and v: sig = int(v)
        if o == '--stamp': stamp = 1
        if o == '--rxddr': DATA_OFF, LOG_RQ = 0x100000, 10
        if o == '--pre' and v: spec = int(v)
        if o == '--maxb' and v: maxb = int(v)
        if o == '--gap' and v: gap = int(v)
    if FRAME < 60 or FRAME > 1500:
        print('--len must be 60..1500', file=sys.stderr)
        return 2
    if (inl & 0xffff) > 222:
        inl = (inl & ~0xffff) | 222
    if lat:
        sig = 1
    sig = max(sig, 1)
    n = min(n, 1000000)

    def flen(i):
        return LENS[i % 12] if MIX else FRAME
    q = np.arange(1536, dtype=np.int64)

    def make_frame(i):
        ln = flen(i)
        f = ((q[:ln] * 13 + i * 5 + (q[:ln] >> 8)) & 0xff).astype(np.uint8)
        f[0:6] = 0xFF
        f[6:12] = np.frombuffer(b'\x02\x49\x4e\x4e\x32\x04', np.uint8)
        f[12], f[13] = ET_IN >> 8, ET_IN & 0xFF
        f[14], f[15], f[16], f[17] = 0xEE, i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF
        return f

    def expect_echo(i):
        f = make_frame(i)
        f[6], f[11], f[12], f[13] = 0x02, 0xEC, ET_OUT >> 8, ET_OUT & 0xFF
        return f.tobytes()

    V, L = fld.env_ifc()
    bdf = argv[1]
    card = fld.Card(bdf, argv[2])
    bo = card.bo('fld_rx', 0, BUF, p2p=True)
    pbo = card.bo('fld_tx', 2, 4096)
    rres, tres, qres = card.bo('fld_rx', 2, 4096), card.bo('fld_tx', 3, 4096), card.bo('fld_reap', 1, 4096)
    rlog, tlog = card.bo('fld_rx', 15, 4096 * 64), card.bo('fld_tx', 22, 4096 * 64)
    if bo.address() != 0x200000000:
        print('*** ring BO at 0x%x, not bank0 offset 0 (URAM window)' % bo.address())
        return 1
    net1 = os.listdir('/sys/class/infiniband/%s/device/net' % argv[4])[0]
    pw = fld.PeerWindow(bdf)
    lay = dict(UMEM_OFF=UMEM_OFF, UMEM_LEN=UMEM_LEN, RQ_OFF=RQ_OFF, RQ_DBR=RQ_DBR, RCQ_OFF=RCQ_OFF, RCQ_DBR=RCQ_DBR,
               SQ_OFF=SQ_OFF, SQ_DBR=SQ_DBR, TCQ_OFF=TCQ_OFF, TCQ_DBR=TCQ_DBR, LOG_RQ=LOG_RQ, LOG_RCQ=LOG_RCQ,
               LOG_SQ=LOG_SQ, LOG_TCQ=LOG_TCQ, LOG_TXS=LOG_TXS, DATA_OFF=DATA_OFF, TXB_OFF=TXB_OFF, STRIDE=STRIDE,
               STOP_OFFS=[STOP_OFF])
    try:
        f0 = fld.FldPort(V, L, bo, argv[3], argv[5], pw, pbo.address(), lay, dict(ethertype=ET_IN))
    except devx.DevxError as e:
        print('*** RX/TX queues: %s' % e)
        return 1
    w0 = fld.peer_window0(pw, f0.uar_bus)
    print('E5.1 FLD-lite: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%x via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, %s' %
          (f0.rx['rqn'], f0.rx['cqn'], f0.sq['sqn'], f0.tcq['cqn'], f0.uar_bus, f0.db_off, 'on' if bf else 'off', inl, sig,
           'lengths 60..1500 mixed' if MIX else '%u B frames' % FRAME))
    rx_args = {0: bo, 1: bo, 2: rres, 15: rlog}
    for k, v in enumerate([UMEM_OFF + RCQ_OFF, LOG_RCQ, UMEM_OFF + RCQ_DBR, UMEM_OFF + RQ_DBR, LOG_RQ, DATA_OFF, STRIDE,
                           STOP_OFF, 16, spec, stamp]):
        rx_args[4 + k] = v
    tx_args = {0: bo, 1: bo, 2: pbo, 3: tres, 22: tlog, 25: maxb, 12: f0.buf_va}
    for k, v in enumerate([UMEM_OFF + SQ_OFF, LOG_SQ, UMEM_OFF + SQ_DBR, f0.sq['sqn'], f0.dmr.lkey, TXB_OFF]):
        tx_args[6 + k] = v
    for k, v in enumerate([LOG_TXS, STRIDE, f0.db_off, bf, 256, inl, sig, STOP_OFF, stamp]):
        tx_args[13 + k] = v
    rp_args = {0: bo, 1: qres}
    for k, v in enumerate([UMEM_OFF + TCQ_OFF, LOG_TCQ, UMEM_OFF + TCQ_DBR, STOP_OFF, gap]):
        rp_args[3 + k] = v
    rrx, rtx, rrp = (fld.run_args(card.kernel(kn), ar) for kn, ar in (('fld_rx', rx_args), ('fld_tx', tx_args), ('fld_reap', rp_args)))
    rrp.start(); rtx.start(); rrx.start()
    ok64, v64 = fld.check_arg64(card, pw, 'fld_tx', 12, f0.buf_va)
    if ok64 is False:
        print('*** fld_tx buf_va reads 0x%x, set 0x%x (high half not zero)' % (v64, f0.buf_va))

    # port 1: traffic generator + echo checker, through the kernel (AF_PACKET on port 1's netdev)
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ET_OUT))
    s.bind((net1, ET_OUT))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 << 20)
    s.setblocking(False)
    rxb = bytearray(4096)
    sent = got = good = ooo = next_id = 0
    ts_tx, ts_rx = [0] * n, [0] * n

    def drain():
        nonlocal got, good, ooo, next_id
        k = 0
        while True:
            try:
                ln = s.recv_into(rxb)
            except BlockingIOError:
                return k
            t = time.monotonic_ns()
            i = rxb[15] | (rxb[16] << 8) | (rxb[17] << 16)
            if i < n:
                good += ln == flen(i) and bytes(rxb[:ln]) == expect_echo(i)
                ts_rx[i] = t
            ooo += i != next_id
            next_id = i + 1
            got += 1
            k += 1
    t0 = time.monotonic()
    rtt = []
    while sent < n:
        ts = time.monotonic_ns()
        fr = make_frame(sent).tobytes()
        while True:
            try:
                s.send(fr)
                break
            except (BlockingIOError, OSError):
                drain()
        ts_tx[sent] = ts
        sent += 1
        drain()
        if lat:
            while got < sent and time.monotonic_ns() - ts < 200_000_000:
                select.select([s], [], [], 0.01)
                drain()
            if got == sent:
                rtt.append((ts_rx[sent - 1] - ts) / 1000.0)
        elif not burst:
            time.sleep(2e-6)
    for _ in range(2000):
        if got >= n:
            break
        drain()
        time.sleep(0.001)
    st32(bo.va + STOP_OFF, 1)
    pw.fence()
    s1, s2, s3 = fld.wait(rrx, 10000), fld.wait(rtx, 10000), fld.wait(rrp, 10000)
    secs = time.monotonic() - t0
    r, t, qq = rres.u32(8), tres.u32(8), qres.u32(8)
    dn = fld.dn
    print('fld_rx  (%s): CQEs %u, frames %u, RX errors %u, slot mismatches %u, polls %u' % (dn(s1), r[0], r[1], r[2], r[3], r[4]))
    print('fld_tx  (%s): frames %u, WQEBBs %u, inline %u, buffered %u, credit waits %u, polls %u, batches %u' %
          (dn(s2), t[0], t[1], t[2], t[3], t[4], t[5], t[6]))
    print('fld_reap(%s): TX CQEs %u, error CQEs %u' % (dn(s3), qq[0], qq[1]))
    if qq[1]:
        print('first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x' % (qq[2] >> 28, (qq[2] >> 16) & 0xFF, (qq[2] >> 8) & 0xFF))
    print('peer window: writes forwarded %u' % ((pw[5] - w0) & 0xFFFFFFFF))
    print('port 1 (%s, AF_PACKET): sent %u, echoes received %u, byte-exact %u, out of order %u, RX errors 0, lost %u  (%.3f s)' %
          (net1, sent, got, good, ooo, sent - got, secs))
    if not lat and got > 1:
        a0, b0 = ts_tx[0], max(ts_rx)
        if a0 and b0 > a0:
            d = (b0 - a0) * 1e-9
            nbytes = sum(flen(i) for i in range(n))
            print('rate (host clock, first send -> last echo; the sender is Python + the kernel): %.0f frames/s, %.2f Gb/s of frames each way' %
                  (got / d, nbytes * 8 / d * 1e-9))
    if lat and rtt:
        us = 1000.0 / devx.query_hca_freq_khz(V, L, f0.ctx)
        fp = []
        for i in range(min(n, len(rtt), 1 << LOG_RCQ, 1 << LOG_TCQ)):
            b1, c1 = fld.cqe_ts(bo.mem, UMEM_OFF + RCQ_OFF + i * 64), fld.cqe_ts(bo.mem, UMEM_OFF + TCQ_OFF + i * 64)
            if b1 and c1 and b1 <= c1:
                fp.append((c1 - b1) * us)
        print('breakdown (%d frames; port 1 through the kernel, host clock):' % len(fp))
        fld.st3(rtt, 'round trip (host send -> host receive)')
        fld.st3(fp, 'FPGA echo: port0 RX CQE -> TX CQE')
        print('  (wire out / wire back: no NIC timestamps on an AF_PACKET socket; e5_host splits them)')
    ok = got == n and good == n and r[1] == n and t[0] == n and not r[2] and not qq[1] and s1 and s2 and s3
    print('RESULT %s (E5.1 FLD-lite: fld_rx -> afu_loop -> fld_tx, no CPU in the data path)' % ('PASS' if ok else 'FAIL'), flush=True)
    pw[1] = 0
    return 0 if ok else 1


if __name__ == '__main__':
    rc = main(sys.argv)
    sys.stdout.flush()
    os._exit(rc)
