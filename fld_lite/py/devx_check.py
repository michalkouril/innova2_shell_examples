#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""devx_check.py <rdma dev> -- host-only check of the Python DevX layer (no FPGA, no traffic): on the ConnectX port
<rdma dev> (e.g. mlx5_1), with rings in host memory, create the objects every FLD-lite host creates -- a TX CQ, an SQ
(transport domain, TIS, SQ RST -> RDY) and an RX path (CQ, RQ RST -> RDY, direct TIR, IPv4/UDP steering rule with a
flow TAG) -- query each back with the QUERY_* commands and compare the fields with what was set, then destroy them
(the steering rule goes with the device context). Prints DEVX-CHECK PASS / FAIL. Needs root (or RDMA rights).
"""
import ctypes as C
import mmap
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devx
import ifc
import verbs

dev = sys.argv[1] if len(sys.argv) > 1 else 'mlx5_1'
V, L = verbs.Verbs(), ifc.Ifc()
print('mlx5_ifc.h: %s (%d layouts)' % (L.path, len(L.types)))
ctx = V.open_dev(dev, True)
if not ctx:
    sys.exit('*** cannot open %s with DevX (errno %d)' % (dev, V.errno()))
pd = V.ibv_alloc_pd(ctx)
mem = mmap.mmap(-1, 0x60000)                       # host memory umem: the rings' layout of the FLD-lite hosts
umem = V.umem_reg(ctx, verbs.buf_addr(mem), 0x60000)
uar = V.alloc_uar(ctx)
if not (pd and umem and uar):
    sys.exit('*** pd / umem / uar allocation failed (errno %d)' % V.errno())
pdn = V.pdn(pd)
print('pd %d, umem %d, uar page %d, min_wqe_inline_mode %d, core clock %d kHz' %
      (pdn, umem.umem_id, uar.page_id, devx.query_min_inline(V, L, ctx), devx.query_hca_freq_khz(V, L, ctx)))
ok = True


def query(obj, typ, idfield, idval, outtyp):
    b = L.buf(typ)
    L.set(b, typ, 'opcode', L.enum('MLX5_CMD_OP_' + typ[:-3].upper()))
    L.set(b, typ, idfield, idval)
    rc, out = V.obj_query(obj, b, L.size(outtyp))
    if rc:
        raise devx.DevxError('%s: errno %d syndrome 0x%x' % (typ, V.errno(), L.get(out, outtyp, 'syndrome')))
    return out


def check(what, got, want):
    global ok
    good = got == want
    ok &= good
    print('  %-34s %-10s %s' % (what, hex(got), 'ok' if good else '*** want %s' % hex(want)))


tcq = devx.cq_create(V, L, ctx, uar.page_id, umem.umem_id, 0x40000, 0x40c0, 11)
sq = devx.sq_create(V, L, ctx, pdn, tcq['cqn'], uar.page_id, umem.umem_id, 0x10000, umem.umem_id, 0x4080, 10,
                    devx.query_min_inline(V, L, ctx))
rx = devx.rx_create(V, L, ctx, pdn, uar.page_id, umem.umem_id, 0x20000, 0x4040, 0x0, 0x4000, 11, 8, 0x0800,
                    udp=True, dst_ip=0xC0A84DFE, udp_dport=9, tag=0x5109)
print('created: TX cqn 0x%x | td 0x%x tis 0x%x sqn 0x%x | RX td 0x%x cqn 0x%x rqn 0x%x tirn 0x%x, flow rule %s' %
      (tcq['cqn'], sq['tdn'], sq['tisn'], sq['sqn'], rx['tdn'], rx['cqn'], rx['rqn'], rx['tirn'], 'yes' if rx['flow'] else 'no'))
o = query(tcq['cq'], 'query_cq_in', 'cqn', tcq['cqn'], 'query_cq_out')
c = L.off('query_cq_out', 'cq_context')
print('TX CQ')
check('log_cq_size', L.get(o, 'cqc', 'log_cq_size', c), 11)
check('uar_page', L.get(o, 'cqc', 'uar_page', c), uar.page_id)
check('c_eqn (event queue)', L.get(o, 'cqc', 'c_eqn_or_apu_element', c), tcq['eqn'])
o = query(sq['sq'], 'query_sq_in', 'sqn', sq['sqn'], 'query_sq_out')
q, w = L.off('query_sq_out', 'sq_context'), L.off('query_sq_out', 'sq_context.wq')
print('SQ')
check('state (RDY)', L.get(o, 'sqc', 'state', q), L.enum('MLX5_SQC_STATE_RDY'))
check('cqn', L.get(o, 'sqc', 'cqn', q), tcq['cqn'])
check('tis_num_0', L.get(o, 'sqc', 'tis_num_0', q), sq['tisn'])
check('wq.pd', L.get(o, 'wq', 'pd', w), pdn)
check('wq.log_wq_sz', L.get(o, 'wq', 'log_wq_sz', w), 10)
check('wq.log_wq_stride', L.get(o, 'wq', 'log_wq_stride', w), 6)
o = query(rx['rq'], 'query_rq_in', 'rqn', rx['rqn'], 'query_rq_out')
q, w = L.off('query_rq_out', 'rq_context'), L.off('query_rq_out', 'rq_context.wq')
print('RQ')
check('state (RDY)', L.get(o, 'rqc', 'state', q), L.enum('MLX5_RQC_STATE_RDY'))
check('cqn', L.get(o, 'rqc', 'cqn', q), rx['cqn'])
check('vsd', L.get(o, 'rqc', 'vsd', q), 1)
check('wq.log_wq_sz', L.get(o, 'wq', 'log_wq_sz', w), 8)
check('wq.log_wq_stride', L.get(o, 'wq', 'log_wq_stride', w), 4)
o = query(rx['tir'], 'query_tir_in', 'tirn', rx['tirn'], 'query_tir_out')
q = L.off('query_tir_out', 'tir_context')
print('TIR')
check('inline_rqn', L.get(o, 'tirc', 'inline_rqn', q), rx['rqn'])
check('transport_domain', L.get(o, 'tirc', 'transport_domain', q), rx['tdn'])
ok &= bool(rx['flow'])
# destroy (dependants first); the steering rule and the rest go with the context
for h in (sq['sq'], sq['tis'], sq['td'], tcq['cq']):
    if V.obj_destroy(h):
        print('*** destroy failed: errno %d' % V.errno())
        ok = False
V.ibv_close_device(ctx)
print('DEVX-CHECK %s' % ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
