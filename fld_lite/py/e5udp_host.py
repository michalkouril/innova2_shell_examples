#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""e5udp_host.py -- the no-compile (Python: pyxrt + ctypes on libibverbs / libmlx5) twin of ../e5/e5udp_host.cpp,
same options, same set-up, same output lines and verdict: FLD-lite as a UDP endpoint on one ConnectX port. DevX
RQ / RX CQ / TIR + SQ / TX CQ with their rings in the bank0 URAM window, frames in FPGA memory, doorbells through
the PEER window; the NIC-RX rule is {IPv4, UDP, dst ip, dst port, !frag} + flow TAG. No traffic generator: the
engines run for --secs while an external client (Linux sockets on another port) sends datagrams; then the counters.
The host only sets things up and waits, so Python costs nothing at run time: the data path has no CPU in it.

usage (root): e5udp_host.py <user BDF> <xclbin> <fld dev> <cx5 BDF of that port> [--secs S] [--ip A.B.C.D] [--port P]
              [--rxk K] [--pre N] [--inline N] [--inline-adapt] [--bf] [--no-rxchk] [--no-csum] [--sig K] [--stamp]
              [--maxb N] [--gap N] [--lat] [--jumbo] [--small] [--plram] [--slots128] [--rxddr]
              [--stop-file F] [--mcd] [--dump N] [--iperf IP,WORDS,GAP[,DELAY_S]]
The options mean what they mean for e5udp_host (see its header): --jumbo 16 KB slots (MTU 9000); --small / --plram
the E5.6 layouts (--plram: fp7 only); --mcd the memcached xclbin (hash table + value store for mcd_mem); --iperf the
fns iperf_udp client (pulsed through iperf_ctl); --lat the per-frame echo time from this port's CQE timestamps.
Needs innova2_peermem (loaded by the run scripts) and OFED's mlx5_ifc.h (fld_lite/py/ifc.py).
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devx  # noqa: E402
import fld  # noqa: E402
from fld import st32  # noqa: E402


def main(argv):
    if len(argv) < 5:
        print('usage: %s <user BDF> <xclbin> <fld dev> <cx5 BDF> [--secs S] [--ip A.B.C.D] [--port P] ...' % argv[0], file=sys.stderr)
        return 2
    BUF, STRIDE = 8 << 20, 2048
    LOG_RQ, LOG_TXS, LOG_RCQ, LOG_TCQ, LOG_SQ = 8, 10, 11, 11, 10
    DATA_OFF, UMEM_OFF, STOP_OFF, TXB_OFF, UMEM_LEN = 0x80000, 0x0, 0x380000, 0x400000, 0x60000
    STOP_RX = STOP_TX = STOP_RP = STOP_OFF
    RQ_OFF, RQ_DBR, RCQ_DBR, SQ_DBR, TCQ_DBR = 0x0, 0x4000, 0x4040, 0x4080, 0x40C0
    SQ_OFF, RCQ_OFF, TCQ_OFF = 0x10000, 0x20000, 0x40000
    ALIAS_OFF, PL_BASE = 0x102000, 0x2000000
    mcd_dump_s, stop_file = 0, None
    secs_run, rxk, port, rxchk, csum, latrep = 60, 4, 7777, 1, 1, 0
    iperf, iperf_ip, iperf_words, iperf_gap, iperf_delay = 0, 0, 23, 0, 5
    ip = 0xC0A84D02                                          # 192.168.77.2
    plram = slots128 = mcdk = False
    bf, inl, sig, stamp, spec, gap, maxb = 0, 0, 16, 0, 2, 64, 16
    a = argv[5:]

    def val(i):
        return a[i + 1] if i + 1 < len(a) else None
    for i, o in enumerate(a):
        v = val(i)
        if o == '--secs' and v: secs_run = int(v)
        if o == '--port' and v: port = int(v)
        if o == '--rxk' and v: rxk = int(v)
        if o == '--ip' and v: ip = int.from_bytes(socket.inet_aton(v), 'big')
        if o == '--no-rxchk': rxchk = 0
        if o == '--no-csum': csum = 0
        if o == '--lat': latrep = 1
        if o == '--iperf' and v:
            f = v.split(',')
            iperf_ip = int.from_bytes(socket.inet_aton(f[0]), 'little')      # s_addr (wire byte order)
            iperf_words = int(f[1]) if len(f) > 1 else 23
            iperf_gap = int(f[2]) if len(f) > 2 else 0
            iperf_delay = int(f[3]) if len(f) > 3 else 5
            iperf = 1
        if o == '--bf': bf = 1
        if o == '--inline' and v: inl = int(v)
        if o == '--inline-adapt': inl |= 1 << 16
        if o == '--sig' and v: sig = int(v)
        if o == '--stamp': stamp = 1
        if o == '--rxddr': DATA_OFF, LOG_RQ = 0x100000, 10
        if o in ('--small', '--plram'):
            LOG_RQ, LOG_RCQ, LOG_TCQ, TCQ_OFF, TCQ_DBR, STOP_RP, STOP_TX = 6, 10, 10, 0x20000, 0x30040, 0x30000, 0x5000
            RCQ_OFF, STOP_RX, DATA_OFF, UMEM_LEN = 0x40000, 0x50000, 0x58000, 0x50000
        if o == '--plram': plram = True
        if o == '--slots128': slots128 = True
        if o == '--mcd': mcdk = True
        if o == '--dump' and v: mcd_dump_s = int(v)
        if o == '--stop-file' and v: stop_file = v
        if o == '--jumbo':
            STRIDE, BUF, DATA_OFF, LOG_RQ, TXB_OFF, LOG_TXS = 16384, 16 << 20, 0x400000, 8, 0x800000, 8
        if o == '--pre' and v: spec = int(v)
        if o == '--maxb' and v: maxb = int(v)
        if o == '--gap' and v: gap = int(v)
    if slots128:
        if DATA_OFF != 0x58000:
            print('--slots128 needs --small or --plram', file=sys.stderr)
            return 2
        LOG_RQ, LOG_RCQ, LOG_TCQ, STRIDE, STOP_RX, DATA_OFF, UMEM_LEN = 7, 8, 8, 1536, 0x44000, 0x48000, 0x44000
    if (inl & 0xffff) > 222:
        inl = (inl & ~0xffff) | 222
    if latrep:
        sig = 1

    V, L = fld.env_ifc()
    bdf = argv[1]
    card = fld.Card(bdf, argv[2])
    # the ring/frame BO first, so it lands at bank0 offset 0 (the HYBRID URAM window); --plram: fld_rx.mem is a PLRAM
    # port, so the bank0 BO goes through fld_tx.buf's group
    bo = card.bo('fld_tx', 1, BUF, p2p=True) if plram else card.bo('fld_rx', 0, BUF, p2p=True)
    if plram:
        pl0, pl1, pl2 = card.bo('fld_rx', 1, 0x20000), card.bo('fld_reap', 0, 0x20000), card.bo('fld_rx', 0, 0x40000)
        if (pl0.address(), pl1.address(), pl2.address()) != (PL_BASE, PL_BASE + 0x20000, PL_BASE + 0x40000):
            print('*** PLRAM BOs at 0x%x/0x%x/0x%x, expected 0x2000000/0x2020000/0x2040000 (is this the --plram xclbin?)' %
                  (pl0.address(), pl1.address(), pl2.address()))
            return 1
    pbo = card.bo('fld_tx', 2, 4096)
    rres, tres, qres = card.bo('fld_rx', 2, 4096), card.bo('fld_tx', 3, 4096), card.bo('fld_reap', 1, 4096)
    rlog, tlog = card.bo('fld_rx', 15, 4096 * 64), card.bo('fld_tx', 22, 4096 * 64)
    if bo.address() != 0x200000000:
        print('*** ring BO at 0x%x, not bank0 offset 0 (URAM window)' % bo.address())
        return 1
    base, va = bo.mem, bo.va
    # --mcd: the memcached memories after the ring BO; mcd_mem is host-started (8 stream args, then ht = 8, vs = 9),
    # started now and never waited for -- the free-running mcd core's start-up zeroing waits in the streams for it
    mdbg = None
    if mcdk:
        kmem = card.kernel('mcd_mem')
        mht = fld.Bo(card, 64 << 20, kmem.group_id(8))       # 2^20 lines: mcd v5
        mvs = fld.Bo(card, 1 << 30, kmem.group_id(9))        # 524288 x 2 KB slots
        if card.has_kernel('mcd_dbg'):
            kdbg = card.kernel('mcd_dbg')
            mdbg = fld.Bo(card, 4096, kdbg.group_id(2))
            mdbg.zero()
            drun = fld.run_args(kdbg, {2: mdbg})
            drun.start()
        else:
            print('memcached: no mcd_dbg kernel')
        mrun = fld.run_args(kmem, {8: mht, 9: mvs})
        mrun.start()
        print('memcached: mcd_mem started; hash table 64 KB @0x%x, value store 64 MB @0x%x' % (mht.address(), mvs.address()))
    NM = ['', 'rx beats', 'rx words', 'tx words', 'tx responses', 'ht rd cmds', 'ht wr cmds', 'vs rd cmds', 'vs wr cmds',
          '', '', '', '', '', '', '', 'mem ht rd cmds', 'mem ht rd words', 'mem ht wr cmds', 'mem ht wr words',
          'mem vs rd cmds', 'mem vs rd words', 'mem vs wr cmds', 'mem vs wr words', 'mem loops']

    def mcd_dump(when):
        if mdbg is None:
            return
        v = mdbg.u64(64)
        print('memcached debug (%s):%s' % (when, ''.join(' %s=%d' % (NM[k], int(v[k]) & 0x00FFFFFFFFFFFFFF)
                                                         for k in range(1, 25) if NM[k])), flush=True)
    if mcdk:
        time.sleep(2)
        mcd_dump('2 s after mcd_mem start, before any traffic')
    pw = fld.PeerWindow(bdf)
    pa = None
    if plram:                                                # the alias on BEFORE any ring is touched
        pa = pw.bar + ALIAS_OFF
        rd = lambda i: fld.ld32(pa + 4 * i)                  # noqa: E731
        wr = lambda i, x: st32(pa + 4 * i, x)                # noqa: E731
        if rd(0) != 0x50414C49:
            print('*** p2p_alias not found at BAR0+0x102000 -- is this card on fp7?')
            return 1
        wr(1, 0); time.sleep(0.001); wr(2, 0); wr(3, 0); wr(8, 0); wr(1, 1); time.sleep(0.001)
        if rd(1) != 0x101:
            print('*** p2p_alias did not enable (CTRL 0x%x)' % rd(1))
            return 1
        print('E5.6: p2p_alias ON, window = BAR offset 0 (512 KB) -> PLRAM0/1/2; rings, CQs, doorbell records, RX slots in PLRAM')

    lay = dict(UMEM_OFF=UMEM_OFF, UMEM_LEN=UMEM_LEN, RQ_OFF=RQ_OFF, RQ_DBR=RQ_DBR, RCQ_OFF=RCQ_OFF, RCQ_DBR=RCQ_DBR,
               SQ_OFF=SQ_OFF, SQ_DBR=SQ_DBR, TCQ_OFF=TCQ_OFF, TCQ_DBR=TCQ_DBR, LOG_RQ=LOG_RQ, LOG_RCQ=LOG_RCQ,
               LOG_SQ=LOG_SQ, LOG_TCQ=LOG_TCQ, LOG_TXS=LOG_TXS, DATA_OFF=DATA_OFF, TXB_OFF=TXB_OFF, STRIDE=STRIDE,
               STOP_OFFS=[STOP_RX, STOP_TX, STOP_RP])
    tag = 0x5100 | (port & 0xff)
    try:
        f0 = fld.FldPort(V, L, bo, argv[3], argv[4], pw, pbo.address(), lay,
                         dict(ethertype=0x0800, udp=True, dst_ip=ip, udp_dport=port, tag=tag))
    except devx.DevxError as e:
        print('*** RX/TX queues: %s' % e)
        return 1
    w0 = fld.peer_window0(pw, f0.uar_bus)
    print('E5.1 FLD-lite: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%x via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, %s' %
          (f0.rx['rqn'], f0.rx['cqn'], f0.sq['sqn'], f0.tcq['cqn'], f0.uar_bus, f0.db_off, 'on' if bf else 'off', inl, sig, '1024 B frames'))

    # engines: stream args (fld_rx 3; fld_tx 4,5; fld_reap 2) are not set by the host
    # step 1 option bits: fld_rx pre = npre | K << 8 | rxchk << 17; fld_tx maxb bits 31:24 = cs_flags
    pre = spec | (rxk << 8) | (rxchk << 17)
    maxb_s1 = maxb | ((0xC0 << 24) if csum else 0)
    k2, k1 = (0x40000, 0x20000) if plram else (0, 0)
    rx_args = {0: pl2 if plram else bo, 1: pl0 if plram else bo, 2: rres, 15: rlog}
    for n, v in enumerate([UMEM_OFF + RCQ_OFF - k2, LOG_RCQ, UMEM_OFF + RCQ_DBR, UMEM_OFF + RQ_DBR, LOG_RQ, DATA_OFF - k2,
                           STRIDE, STOP_RX - k2, 16, pre, stamp]):
        rx_args[4 + n] = v
    tx_args = {0: pl0 if plram else bo, 1: bo, 2: pbo, 3: tres, 22: tlog, 25: maxb_s1}
    for n, v in enumerate([UMEM_OFF + SQ_OFF, LOG_SQ, UMEM_OFF + SQ_DBR, f0.sq['sqn'], f0.dmr.lkey, TXB_OFF]):
        tx_args[6 + n] = v
    tx_args[12] = f0.buf_va
    for n, v in enumerate([LOG_TXS, STRIDE, f0.db_off, bf, 256, inl, sig, STOP_TX, stamp]):
        tx_args[13 + n] = v
    rp_args = {0: pl1 if plram else bo, 1: qres}
    for n, v in enumerate([UMEM_OFF + TCQ_OFF - k1, LOG_TCQ, UMEM_OFF + TCQ_DBR - k1, STOP_RP - k1, gap]):
        rp_args[3 + n] = v
    rrx, rtx, rrp = fld.run_args(card.kernel('fld_rx'), rx_args), fld.run_args(card.kernel('fld_tx'), tx_args), \
        fld.run_args(card.kernel('fld_reap'), rp_args)
    rrp.start(); rtx.start(); rrx.start()
    ok64, v64 = fld.check_arg64(card, pw, 'fld_tx', 12, f0.buf_va)
    if ok64 is False:
        print('*** fld_tx buf_va reads 0x%x, set 0x%x (high half not zero)' % (v64, f0.buf_va), flush=True)
    print('UDP echo endpoint UP: %s:%u on %s (tag 0x%x), rx checksum drop %s, tx cs_flags %s, pre 0x%x, maxb 0x%x -- %u s' %
          (socket.inet_ntoa(ip.to_bytes(4, 'big')), port, argv[3], tag, 'on' if rxchk else 'off', 'L3|L4' if csum else 'off',
           pre, maxb_s1, secs_run), flush=True)
    t0 = time.monotonic()
    if iperf:                                                # fns iperf_udp's runExperiment is level-sensitive: pulse it
        time.sleep(iperf_delay)
        kc = card.kernel('iperf_ctl')
        for run, pause in ((1, 0.5), (0, 0)):
            r = fld.run_args(kc, {1: run, 2: iperf_words, 3: iperf_gap, 4: iperf_ip})
            r.start(); r.wait()
            time.sleep(pause)
        print('iperf_udp started: %u x 64 B datagrams (%u B payload), gap %u cycles, target %s:5001 (from port 32768)' %
              (iperf_words, iperf_words * 64, iperf_gap, socket.inet_ntoa(iperf_ip.to_bytes(4, 'little'))), flush=True)
    el = 0
    while time.monotonic() - t0 < secs_run:
        time.sleep(1)
        if stop_file and os.path.exists(stop_file):
            print('stop file %s seen -- ending the run' % stop_file)
            break
        if mcdk and mcd_dump_s and el % mcd_dump_s == mcd_dump_s - 1:
            mcd_dump('t=%d' % int(time.time()))
        el += 1
    if mcdk:
        mcd_dump('end of run, before stopping FLD-lite')
    for so in (STOP_RX, STOP_TX, STOP_RP):
        st32(va + so, 1)
    pw.fence()
    s1, s2, s3 = fld.wait(rrx, 10000), fld.wait(rtx, 10000), fld.wait(rrp, 10000)
    r, t, q = rres.u32(16), tres.u32(16), qres.u32(16)
    print('fld_rx  (%s): CQEs %u, frames %u, RX drops %u (of which checksum/options %u), slot mismatches %u, polls %u' %
          (fld.dn(s1), r[0], r[1], r[2], r[8], r[3], r[4]))
    print('fld_tx  (%s): frames %u, WQEBBs %u, inline %u, buffered %u, credit waits %u, batches %u' %
          (fld.dn(s2), t[0], t[1], t[2], t[3], t[4], t[6]))
    print('fld_reap(%s): TX CQEs %u, error CQEs %u' % (fld.dn(s3), q[0], q[1]))
    if q[1]:
        print('first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x' % (q[2] >> 28, (q[2] >> 16) & 0xFF, (q[2] >> 8) & 0xFF))
    print('peer window: writes forwarded %u' % ((pw[5] - w0) & 0xFFFFFFFF))
    if latrep:
        # FPGA echo per frame from this card's own CQE timestamps: RX CQE j and TX CQE j belong to the same frame when
        # every WQE is signalled (--sig 1) and nothing was dropped; grouped by length
        us = 1000.0 / devx.query_hca_freq_khz(V, L, f0.ctx)
        by, bad = {}, 0
        nf = min(int(r[1]), 1 << LOG_RCQ, 1 << LOG_TCQ)
        for j in range(nf if r[2] == 0 else 0):
            rc, tc = UMEM_OFF + RCQ_OFF + j * 64, UMEM_OFF + TCQ_OFF + j * 64
            x, y = fld.cqe_ts(base, rc), fld.cqe_ts(base, tc)
            ln = int.from_bytes(base[rc + 44:rc + 48].tobytes(), 'big')
            if not x or not y or y <= x:
                bad += 1
                continue
            by.setdefault(ln, []).append((y - x) * us)
        print('FPGA echo (this card\'s RX CQE -> TX CQE, %u frames%s):' %
              (nf, '; skipped: frames were dropped, indices would not pair' if r[2] else ''))
        for ln in sorted(by):
            fld.st3(by[ln], 'frame %u B (%d)' % (ln, len(by[ln])))
        if bad:
            print('  (%u pairs without usable timestamps)' % bad)
    if pa:
        print('E5.6: p2p_alias redirected %u writes, %u reads; switching it off' % (fld.ld32(pa + 32), fld.ld32(pa + 36)))
        st32(pa + 4, 0)
        time.sleep(0.001)
    ok = s1 and s2 and s3 and not q[1] and t[0] == r[1]
    print('E5UDP-HOST %s (engines stopped cleanly, no TX error CQEs, every accepted frame echoed)' % ('OK' if ok else 'PROBLEM'), flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    rc = main(sys.argv)
    sys.stdout.flush()
    os._exit(rc)        # skip the interpreter's teardown: free-running runs (mcd_mem) would abort XRT's destructors
