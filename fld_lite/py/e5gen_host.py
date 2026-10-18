#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""e5gen_host.py -- the no-compile (Python: pyxrt + ctypes on libibverbs / libmlx5) twin of ../e5/e5gen_host.cpp,
same options, same set-up, same output lines and verdict. FLD-lite and its AFU (afu_loop, or ZUC with --zuc) on one
ConnectX port; the other port is driven BY THE FPGA: gen_tx posts frames on its SQ (rings in a second FPGA memory
region, doorbells through PEER window 1) and sink_rx receives the replies on its RQ, counts them and byte-checks a
sample against precomputed replies. No host CPU in the data path, so the rates are the C host's: Python only builds
the templates and the expected replies (ZUC: fld_lite/py/zuc_model.py) and sets up the queues.

usage (root): e5gen_host.py <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF of port0> [frames] [options]
  --len L | --mix   frame length (default 1024) or K templates of mixed lengths
  --gap N  gen_tx pacing; --gbatch B WQEs per doorbell (16); --k K templates (64); --gbf; --gnozero
  --sample S  sink_rx checks every 2^S-th frame; --swap  FLD-lite on port 1; --gfrm-uram; --gring-uram; --jumbo
  --zuc [--op conf|integ|mix] [--zlen L]   the ZUC AFU xclbin
  FLD-lite options as e5_host: --bf --inline N --inline-adapt --sig K --pre N --gap-reap N --maxb N --rxk K --linger N
  --stamp --rxddr --lat --burst (accepted, as the C host)
Environment: GEN_SIG (gen_tx signalling interval, default 64), FLD_NO_ZERO (fld_reap without zero-on-reap).
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devx  # noqa: E402
import fld  # noqa: E402
import verbs  # noqa: E402
from fld import ET_IN, ET_OUT, st32, be32  # noqa: E402

LENS = (60, 64, 100, 128, 200, 222, 223, 256, 511, 1000, 1500, 777)
ZLENS = (1, 13, 64, 65, 100, 128, 200, 511, 1000, 1372)
ZLENS_J = (1, 64, 511, 1372, 2048, 3000, 4096, 6000, 8000, 8192)


def zrng(i, k):
    x = ((i * 2654435761) ^ (k * 40503 + 0x9e3779b9)) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x2c1b3c6d) & 0xFFFFFFFF
    x ^= x >> 12
    return x


def main(argv):
    if len(argv) < 6:
        print('usage: %s <user BDF> <xclbin> <port0 dev> <port1 dev> <cx5 BDF> [frames] [options]' % argv[0], file=sys.stderr)
        return 2
    n = int(argv[6]) if len(argv) > 6 and argv[6][0] != '-' else 1000
    BUF = BUF2 = 8 << 20
    STRIDE = FSTR = 2048
    LOG_RQ, LOG_TXS, LOG_RCQ, LOG_SQ, LOG_TCQ = 8, 10, 11, 10, 11
    DATA_OFF, UMEM_OFF, UMEM_LEN, STOP_OFF, TXB_OFF = 0x80000, 0x0, 0x60000, 0x380000, 0x400000
    RQ_OFF, RQ_DBR, RCQ_DBR, SQ_DBR, TCQ_DBR = 0x0, 0x4000, 0x4040, 0x4080, 0x40C0
    SQ_OFF, RCQ_OFF, TCQ_OFF = 0x10000, 0x20000, 0x40000
    G_RQ, G_RQ_DBR, G_RCQ_DBR, G_SQ_DBR, G_TCQ_DBR = 0x0, 0x4000, 0x4040, 0x4080, 0x40C0
    G_SQ, G_RCQ, G_TCQ, G_UMEM_LEN, G_TMPL, G_ELEN = 0x10000, 0x20000, 0x60000, 0x80000, 0xA0000, 0xA8000
    G_STOP, G_DATA, G_FRM, G_EXP = 0xB0000, 0x100000, 0x400000, 0x600000
    G_LOG_RQ, G_LOG_RCQ, G_LOG_SQ, G_LOG_TCQ = 10, 12, 10, 11
    FRAME, MIX = 1024, False
    lat = False
    bf, inl, sig, stamp, spec, gap, maxb, rxk, linger = 0, 0, 16, 0, 2, 64, 16, 0, 0
    gflags, ggap, gbatch, log_k, log_sample, zlen = 0, 0, 16, 6, 0, 0
    zuc = jumbo = guram = swapp = gfu = False
    zop = -1
    a = argv[6:]
    for i, o in enumerate(a):
        v = a[i + 1] if i + 1 < len(a) else None
        if o == '--lat': lat = True
        if o == '--bf': bf = 1
        if o == '--mix': MIX = True
        if o == '--inline' and v: inl = int(v)
        if o == '--inline-adapt': inl |= 1 << 16
        if o == '--len' and v: FRAME = int(v)
        if o == '--sig' and v: sig = int(v)
        if o == '--stamp': stamp = 1
        if o == '--rxddr': DATA_OFF, LOG_RQ = 0x100000, 10
        if o == '--pre' and v: spec = int(v)
        if o == '--rxk' and v: rxk = int(v)
        if o == '--linger' and v: linger = int(v)
        if o == '--maxb' and v: maxb = int(v)
        if o == '--gap-reap' and v: gap = int(v)
        if o == '--gap' and v: ggap = int(v)
        if o == '--gbatch' and v: gbatch = int(v)
        if o == '--gbf': gflags |= 1
        if o == '--gnozero': gflags |= 2
        if o == '--k' and v:
            kk, log_k = int(v), 0
            while (2 << log_k) <= kk and log_k < 8:
                log_k += 1
        if o == '--sample' and v: log_sample = int(v)
        if o == '--zuc': zuc = True
        if o == '--jumbo': jumbo = True
        if o == '--gring-uram': guram = True
        if o == '--swap': swapp = True
        if o == '--gfrm-uram': gfu = True
        if o == '--zlen' and v: zlen = int(v)
        if o == '--op' and v: zop = 0 if v == 'conf' else 1 if v == 'integ' else -1
    if jumbo:
        STRIDE = FSTR = 16384
        BUF = BUF2 = 16 << 20
        DATA_OFF, LOG_RQ, TXB_OFF, LOG_TXS = 0x400000, 8, 0x800000, 8
        G_LOG_RQ, G_FRM, G_EXP = 8, 0x800000, 0xC00000
        if log_k > 8:
            print('--jumbo allows at most 256 templates', file=sys.stderr)
            return 2
    if gfu and (not jumbo or guram or log_k > 5):
        print('--gfrm-uram needs --jumbo, not --gring-uram, and at most 32 templates (--k 32)', file=sys.stderr)
        return 2
    if guram and not jumbo:
        print('--gring-uram needs --jumbo (it uses the URAM space --jumbo frees)', file=sys.stderr)
        return 2
    if FRAME < 60 or FRAME > (9000 if jumbo else 1500):
        print('--len must be 60..%u' % (9000 if jumbo else 1500), file=sys.stderr)
        return 2
    if zuc and zlen > (8192 if jumbo else 1372):
        print('--zlen must be <= %u%s' % (8192 if jumbo else 1372, '' if jumbo else ' (use --jumbo for more)'), file=sys.stderr)
        return 2
    if (inl & 0xffff) > 222:
        inl = (inl & ~0xffff) | 222
    if lat:
        sig = 1
    sig = max(sig, 1)
    n = min(n, 100000000)

    V, L = fld.env_ifc()
    bdf = argv[1]
    card = fld.Card(bdf, argv[2])
    bo = card.bo('fld_rx', 0, BUF, p2p=True)              # first, so it lands at bank0 offset 0 (the URAM window)
    pbo = card.bo('fld_tx', 2, 4096)
    rres, tres, qres = card.bo('fld_rx', 2, 4096), card.bo('fld_tx', 3, 4096), card.bo('fld_reap', 1, 4096)
    rlog, tlog = card.bo('fld_rx', 15, 4096 * 64), card.bo('fld_tx', 22, 4096 * 64)
    if bo.address() != 0x200000000:
        print('*** ring BO at 0x%x, not bank0 offset 0 (URAM window)' % bo.address())
        return 1
    bo2 = card.bo('gen_tx', 0, BUF2, p2p=True)
    gres, sres = card.bo('gen_tx', 2, 4096), card.bo('sink_rx', 2, 4096)
    base, g = bo.mem, bo2.mem
    # generator / sink rings in region 2 (DDR) by default; --gring-uram: in region 1's URAM window at 0x80000, and the
    # templates / lengths / stop word in region 1's free DDR at 0xC00000; gen_tx and sink_rx then address region 1
    grg_m, grg_o, grg_va = g, 0, bo2.va
    gt_m, gt_o, gt_va = g, 0, bo2.va
    kbo, GR_OFF, T_SHIFT, D2 = bo2, 0, 0, 0
    if guram:
        if bo2.address() <= bo.address() or bo2.address() - bo.address() > 0xF0000000:
            print('*** region 2 not addressable from region 1')
            return 1
        grg_m, grg_o, grg_va = base, 0x80000, bo.va + 0x80000
        GR_OFF, T_SHIFT = 0x80000, 0xC00000 - G_TMPL
        gt_m, gt_o, gt_va = base, T_SHIFT, bo.va + T_SHIFT
        kbo, D2 = bo, bo2.address() - bo.address()

    cx = argv[5]
    bdf_fld, bdf_gen = cx[:-1] + ('1' if swapp else '0'), cx[:-1] + ('0' if swapp else '1')
    c0 = V.open_dev(argv[4] if swapp else argv[3], True)
    c1 = V.open_dev(argv[3] if swapp else argv[4], True)
    if not c0 or not c1:
        print('cannot open devices', file=sys.stderr)
        return 1
    pw = fld.PeerWindow(bdf)
    lay = dict(UMEM_OFF=UMEM_OFF, UMEM_LEN=UMEM_LEN, RQ_OFF=RQ_OFF, RQ_DBR=RQ_DBR, RCQ_OFF=RCQ_OFF, RCQ_DBR=RCQ_DBR,
               SQ_OFF=SQ_OFF, SQ_DBR=SQ_DBR, TCQ_OFF=TCQ_OFF, TCQ_DBR=TCQ_DBR, LOG_RQ=LOG_RQ, LOG_RCQ=LOG_RCQ,
               LOG_SQ=LOG_SQ, LOG_TCQ=LOG_TCQ, LOG_TXS=LOG_TXS, DATA_OFF=DATA_OFF, TXB_OFF=TXB_OFF, STRIDE=STRIDE,
               STOP_OFFS=[STOP_OFF])
    try:
        f0 = fld.FldPort(V, L, bo, None, bdf_fld, pw, pbo.address(), lay, dict(ethertype=ET_IN), ctx=c0)
    except devx.DevxError as e:
        print('*** RX/TX queues: %s' % e)
        return 1
    w0 = fld.peer_window0(pw, f0.uar_bus)
    if pw[8] < 4:
        print('*** the generator needs peer_window v1.1 (4 windows; NWIN reads %u)' % pw[8])
        return 1
    print('E5.1 FLD-lite: RX rqn 0x%x rcqn 0x%x | TX sqn 0x%x tcqn 0x%x | UAR 0x%x via PEER +0x%x | BlueFlame %s, inline <= %u, sig every %u, %s' %
          (f0.rx['rqn'], f0.rx['cqn'], f0.sq['sqn'], f0.tcq['cqn'], f0.uar_bus, f0.db_off, 'on' if bf else 'off', inl, sig,
           'lengths 60..1500 mixed' if MIX else '%u B frames' % FRAME))
    rx_args = {0: bo, 1: bo, 2: rres, 15: rlog}
    for k, v in enumerate([UMEM_OFF + RCQ_OFF, LOG_RCQ, UMEM_OFF + RCQ_DBR, UMEM_OFF + RQ_DBR, LOG_RQ, DATA_OFF, STRIDE,
                           STOP_OFF, 16, (spec & 0xff) | ((rxk & 0xff) << 8), stamp]):
        rx_args[4 + k] = v
    tx_args = {0: bo, 1: bo, 2: pbo, 3: tres, 22: tlog, 25: (maxb & 0xff) | (linger << 8), 12: f0.buf_va}
    for k, v in enumerate([UMEM_OFF + SQ_OFF, LOG_SQ, UMEM_OFF + SQ_DBR, f0.sq['sqn'], f0.dmr.lkey, TXB_OFF]):
        tx_args[6 + k] = v
    for k, v in enumerate([LOG_TXS, STRIDE, f0.db_off, bf, 256, inl, sig, STOP_OFF, stamp]):
        tx_args[13 + k] = v
    rp_args = {0: bo, 1: qres}
    for k, v in enumerate([UMEM_OFF + TCQ_OFF, LOG_TCQ, UMEM_OFF + TCQ_DBR, STOP_OFF, gap, UMEM_OFF + SQ_OFF,
                           0 if os.environ.get('FLD_NO_ZERO') else LOG_SQ]):            # zero-on-reap
        rp_args[3 + k] = v
    rrx, rtx, rrp = (fld.run_args(card.kernel(kn), ar) for kn, ar in (('fld_rx', rx_args), ('fld_tx', tx_args), ('fld_reap', rp_args)))
    rrp.start(); rtx.start(); rrx.start()
    ok64, v64 = fld.check_arg64(card, pw, 'fld_tx', 12, f0.buf_va)
    if ok64 is False:
        print('*** fld_tx buf_va reads 0x%x, set 0x%x (high half not zero)' % (v64, f0.buf_va))

    # ---------------------------------------------------------------- port 1: the FPGA traffic generator
    K = 1 << log_k
    pd1 = V.ibv_alloc_pd(c1)
    gmr = V.reg_mr(pd1, bo2.va + G_DATA, min(G_EXP + FSTR * 256, BUF2) - G_DATA)
    gum = V.umem_reg(c1, grg_va, G_UMEM_LEN)
    if not gmr or not gum:
        print('port 1 registration failed: %s' % os.strerror(V.errno()))
        return 1
    fmr = V.reg_mr(pd1, bo.va + 0x80000, 0x80000) if gfu else None
    if gfu and not fmr:
        print('URAM frame registration failed: %s' % os.strerror(V.errno()))
        return 1
    # frame k: gf_m[gf_o + G_FRM + k * FSTR], process address gf_va + G_FRM + k * FSTR
    gf_m, gf_o, gf_va = (base, 0x80000 - G_FRM, bo.va + 0x80000 - G_FRM) if gfu else (g, 0, bo2.va)
    flkey = fmr.lkey if gfu else gmr.lkey
    pdn1 = V.pdn(pd1)
    uar1 = V.alloc_uar(c1)
    grg_m[grg_o:grg_o + G_UMEM_LEN] = 0
    gt_m[gt_o + G_STOP:gt_o + G_STOP + 64] = 0
    grg_m[grg_o + G_RCQ + 63:grg_o + G_RCQ + (64 << G_LOG_RCQ):64] = 0xF1
    grg_m[grg_o + G_TCQ + 63:grg_o + G_TCQ + (64 << G_LOG_TCQ):64] = 0xF1
    for i in range(1 << G_LOG_RQ):
        verbs.set_data_seg(grg_m, grg_o + G_RQ + i * 16, STRIDE, gmr.lkey, bo2.va + G_DATA + i * STRIDE)
    # K frames (echo requests / ZUC requests), WQE templates, expected replies, lengths
    elen = np.zeros(K, np.uint16)
    zpay = 0
    q = np.arange(9216, dtype=np.int64)
    for k in range(K):
        fo = gf_o + G_FRM + k * FSTR
        fva = gf_va + G_FRM + k * FSTR
        e = bytearray(FSTR)
        if zuc:
            import zuc_model as Z
            op = zop if zop >= 0 else (k & 1)
            Ln = zlen if zlen else (ZLENS_J if jumbo else ZLENS)[(k // 2) % 10]
            if op == 1 and Ln > 8191:
                Ln = 8191                                  # INTEG's bit counter is 16 bits
            fl = 128 + Ln
            f = bytearray(FSTR)
            f[0:6] = b'\x02\x49\x4e\x4e\x32\x00'; f[6:12] = b'\x02\x49\x4e\x4e\x32\x04'; f[12], f[13] = ET_IN >> 8, ET_IN & 0xFF
            ip = 14
            f[ip], f[ip + 2], f[ip + 3], f[ip + 5], f[ip + 6], f[ip + 8], f[ip + 9] = 0x45, ((fl - 14) >> 8) & 0xff, (fl - 14) & 0xff, k & 0xff, 0x40, 64, 17
            f[ip + 12], f[ip + 15], f[ip + 16], f[ip + 19] = 10, 2, 10, 1
            cs = sum((f[ip + j] << 8) | f[ip + j + 1] for j in range(0, 20, 2))
            cs = (cs & 0xffff) + (cs >> 16); cs = (cs & 0xffff) + (cs >> 16); cs = ~cs & 0xffff
            f[ip + 10], f[ip + 11] = cs >> 8, cs & 0xff
            f[34:40] = bytes((0x30, 0x39, 0x16, 0x2e, ((fl - 34) >> 8) & 0xff, (fl - 34) & 0xff))
            f[42:64] = b'\xAA' * 22
            h = 64
            f[h], f[h + 1], f[h + 2], f[h + 3] = op, 0x5A, Ln >> 8, Ln & 0xff
            f[h + 4:h + 12] = k.to_bytes(8, 'big')
            for j in range(16):
                f[h + 12 + j], f[h + 28 + j] = zrng(k, j) & 0xff, zrng(k, 100 + j) & 0xff
            f[h + 44:h + 64] = b'\xCC' * 20
            f[128:128 + Ln] = bytes(zrng(k, 1000 + j) & 0xff for j in range(Ln))
            # expected reply: L2-L4 swapped, IP/UDP lengths, both checksums 0, header echo, result
            e[0:6], e[6:12], e[12], e[13] = f[6:12], f[0:6], f[12], f[13]
            e[14:34] = f[14:34]; e[26:30] = f[30:34]; e[30:34] = f[26:30]; e[24] = e[25] = 0
            ipl, udl = (Ln + 0x72, Ln + 0x5E) if op == 0 else (0x62, 0x4E)
            e[16], e[17] = (ipl >> 8) & 0xff, ipl & 0xff
            e[34:36], e[36:38], e[38], e[39] = f[36:38], f[34:36], (udl >> 8) & 0xff, udl & 0xff
            e[64:76] = f[64:76]
            key, iv = bytes(f[h + 12:h + 28]), bytes(f[h + 28:h + 44])
            if op == 0:
                e[128:128 + Ln] = Z.zuc_xor(key, iv, bytes(f[128:128 + Ln]))
                elen[k] = 128 + Ln
            else:
                e[108:112] = Z.eia3_mac(key, iv, bytes(f[128:128 + Ln]), 8 * Ln).to_bytes(4, 'big')
                elen[k] = 112
            zpay += Ln
            gf_m[fo:fo + FSTR] = np.frombuffer(bytes(f), np.uint8)
        else:
            Ln = LENS[k % 12] if MIX else FRAME
            f = ((q[:Ln] * 13 + k * 5 + (q[:Ln] >> 8)) & 0xff).astype(np.uint8)
            f[0:6] = 0xFF
            f[6:12] = np.frombuffer(b'\x02\x49\x4e\x4e\x32\x04', np.uint8)
            f[12], f[13], f[14] = ET_IN >> 8, ET_IN & 0xFF, k & 0xff
            fl = Ln
            gf_m[fo:fo + Ln] = f
            e[0:Ln] = f.tobytes()
            e[6], e[11], e[12], e[13] = 0x02, 0xEC, ET_OUT >> 8, ET_OUT & 0xFF
            elen[k] = Ln
        g[G_EXP + k * FSTR:G_EXP + (k + 1) * FSTR] = np.frombuffer(bytes(e), np.uint8)
        w = bytearray(64)
        w[7], w[29] = 4, 18                               # ctrl ds=4; eth seg: 18-byte inline header
        w[30:48] = gf_m[fo:fo + 18].tobytes()
        w[48:52] = (fl - 18).to_bytes(4, 'big'); w[52:56] = flkey.to_bytes(4, 'big'); w[56:64] = (fva + 18).to_bytes(8, 'big')
        gt_m[gt_o + G_TMPL + k * 64:gt_o + G_TMPL + (k + 1) * 64] = np.frombuffer(bytes(w), np.uint8)
    gt_m[gt_o + G_ELEN:gt_o + G_ELEN + 2 * K] = elen.view(np.uint8)
    pw.fence()
    try:
        grx = devx.rx_create(V, L, c1, pdn1, uar1.page_id, gum.umem_id, G_RCQ, G_RCQ_DBR, G_RQ, G_RQ_DBR, G_LOG_RCQ, G_LOG_RQ,
                             ET_IN if zuc else ET_OUT)     # ZUC replies keep the request ethertype
        st32(grg_va + G_RQ_DBR, be32(1 << G_LOG_RQ))
        gtcq = devx.cq_create(V, L, c1, uar1.page_id, gum.umem_id, G_TCQ, G_TCQ_DBR, G_LOG_TCQ)
        try:
            mi1 = devx.query_min_inline(V, L, c1)
        except devx.DevxError:
            mi1 = 1
        gsq = devx.sq_create(V, L, c1, pdn1, gtcq['cqn'], uar1.page_id, gum.umem_id, G_SQ, gum.umem_id, G_SQ_DBR, G_LOG_SQ, mi1)
    except devx.DevxError as e:
        print('*** port 1: %s' % e)
        return 1
    pw.fence()
    uar1_bus = verbs.bar_start(bdf_gen, 0) + uar1.page_id * 4096 + (uar1.reg_addr - uar1.base_addr)
    db_off1 = ((uar1_bus & 0xFFFFF) + (1 << 20) - (pbo.address() & 0xFFFFF)) & 0xFFFFFFFF
    # PEER window 1 -> port 1 UAR (base writes are ignored while enabled: disable, set, re-enable)
    pw[1] = 0
    pw[0x28 // 4] = uar1_bus & ~0xFFFFF
    pw[0x2C // 4] = uar1_bus >> 32
    pw[1] = 1
    if pw[0x28 // 4] != (uar1_bus & ~0xFFFFF) & 0xFFFFFFFF:
        print('*** peer window 1 base did not stick (0x%08x)' % pw[0x28 // 4])
        return 1
    print('generator: port 1 (%s) sqn 0x%x rqn 0x%x; UAR 0x%x via PEER window 1 +0x%x; K=%u templates, batch %u, gap %u, check 1/%u, BlueFlame %s, zero-on-reap %s' %
          (bdf_gen, gsq['sqn'], grx['rqn'], uar1_bus, db_off1, K, gbatch, ggap, 1 << log_sample,
           ('on' if gbatch == 1 else 'on (single-WQE batches only)') if gflags & 1 else 'off', 'off' if gflags & 2 else 'on'))
    gargs = {0: kbo, 1: pbo, 2: gres}
    for k, v in enumerate([G_TMPL + T_SHIFT, log_k, G_SQ + GR_OFF, G_LOG_SQ, G_SQ_DBR + GR_OFF, gsq['sqn'], G_TCQ + GR_OFF,
                           G_LOG_TCQ, G_TCQ_DBR + GR_OFF, db_off1, n, gbatch, ggap, int(os.environ.get('GEN_SIG', 64)),
                           G_STOP + T_SHIFT, gflags, 256]):
        gargs[3 + k] = v
    sargs = {0: kbo, 1: kbo, 2: sres}
    for k, v in enumerate([G_RCQ + GR_OFF, G_LOG_RCQ, G_RCQ_DBR + GR_OFF, G_RQ_DBR + GR_OFF, G_LOG_RQ, G_DATA + D2, STRIDE,
                           G_EXP + D2, FSTR, G_ELEN + T_SHIFT, log_k, log_sample, n, G_STOP + T_SHIFT, 19 if zuc else 14]):
        sargs[3 + k] = v
    rgen, rsnk = fld.run_args(card.kernel('gen_tx'), gargs), fld.run_args(card.kernel('sink_rx'), sargs)
    pwf0 = pw[5]
    rsnk.start()
    t0 = time.monotonic()
    rgen.start()
    sg = fld.wait(rgen, 60000)
    gsecs = time.monotonic() - t0
    time.sleep(0.3)                                       # let the last frames through
    st32(gt_va + G_STOP, 1)
    st32(bo.va + STOP_OFF, 1)
    pw.fence()
    ss = fld.wait(rsnk, 10000)
    s1, s2, s3 = fld.wait(rrx, 10000), fld.wait(rtx, 10000), fld.wait(rrp, 10000)
    r, t, qq, gr, sr = rres.u32(8), tres.u32(16), qres.u32(8), gres.u32(8), [int(x) for x in sres.u64(10)]
    dn = fld.dn
    print('gen_tx  (%s): posted %u in %u doorbells, TX CQEs %u, TX errors %u, credit polls %u  (%.3f s wall)' %
          (dn(sg), gr[0], gr[1], gr[2], gr[3], gr[5], gsecs))
    if gr[3]:
        print('  first TX error CQE: opcode %u syndrome 0x%02x vendor 0x%02x' % (gr[4] >> 28, (gr[4] >> 16) & 0xFF, (gr[4] >> 8) & 0xFF))
    print('fld_rx  (%s): CQEs %u, frames %u, RX errors %u | fld_tx (%s): frames %u, batches %u | fld_reap (%s): CQEs %u, errors %u, SQ slots zeroed %u' %
          (dn(s1), r[0], r[1], r[2], dn(s2), t[0], t[6], dn(s3), qq[0], qq[1], qq[5]))
    print('fld_rx  polls %u, polls with >1 CQE %u, longest run %u, polls issued with K>1 %u (rxk %u)' % (r[4], r[5], r[6], r[7], rxk))
    print('fld_tx  batches %u, frames per batch %.2f, joined by linger %u, largest batch %u, closed early for room %u, inline frames %u (linger %u)' %
          (t[6], t[0] / t[6] if t[6] else 0.0, t[7], t[8], t[9], t[2], linger))
    print('sink_rx (%s): frames %u, bytes %u, RX error CQEs %u, checked %u, MISMATCHES %u, length mismatches %u' %
          (dn(ss), sr[0], sr[1], sr[2], sr[3], sr[4], sr[9]))
    print('peer window: writes forwarded %u, refused %u (counter totals)' % ((pw[5] - pwf0) & 0xFFFFFFFF, pw[7]))
    us = 1000.0 / devx.query_hca_freq_khz(V, L, c1)
    lost_fld, lost_after = (int(gr[0]) - int(r[0])) & 0xFFFFFFFF, (int(r[1]) - sr[0]) & 0xFFFFFFFF
    print('loss: %u before FLD-lite (port 0 RQ empty), %u after it' % (lost_fld, lost_after))
    if sr[0] > 1 and sr[6] > sr[5]:
        d = (sr[6] - sr[5]) * us * 1e-6
        fps = (sr[0] - 1) / d
        avg = sr[1] / sr[0]
        print('RATE through FLD-lite: %.3f M frames/s, %.2f Gb/s of frames (%.2f Gb/s on the wire incl. FCS/preamble/IFG)' %
              (fps * 1e-6, fps * avg * 8e-9, fps * (avg + 24) * 8e-9))
        if zuc:
            print('ZUC: %.3f M requests/s, %.2f Gb/s of ZUC payload' % (fps * 1e-6, fps * (zpay / K) * 8e-9))
    ok = gr[0] == n and not gr[3] and sr[4] == 0 and sr[9] == 0 and sr[2] == 0 and lost_after == 0 and sr[3] > 0 and \
        sg and ss and s1 and s2 and s3
    print('RESULT %s (FPGA generator -> FLD-lite -> FPGA sink: nothing corrupted, nothing lost after FLD-lite%s)' %
          ('PASS' if ok else 'FAIL', '; loss before it = offered load above FLD-lite\'s rate' if lost_fld else ', no loss at all'), flush=True)
    pw[1] = 0
    return 0 if ok else 1


if __name__ == '__main__':
    rc = main(sys.argv)
    sys.stdout.flush()
    os._exit(rc)        # skip the interpreter's teardown (process exit releases every verbs / DevX / XRT object)
