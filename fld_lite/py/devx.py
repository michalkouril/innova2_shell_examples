# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""devx -- the DevX objects of the FLD-lite hosts, as ../devx/devx_{cq,sq,rx,rx_s0}.c build them, with the command
layouts from the host's mlx5_ifc.h (ifc.Ifc) instead of the compiler. Same objects, same fields, same order:

    cq_create(...)         e3_cq_create   CQ whose ring and doorbell record live in a caller umem
    query_min_inline(...)  e1_query_min_inline
    sq_create(...)         e1_create_sq   transport domain -> TIS -> SQ (RST -> RDY), rings in caller umems
    rx_create(...)         e2_rx_create   transport domain, CQ, cyclic RQ (RST -> RDY), direct TIR, NIC-RX rule; the
                           rule matches one EtherType (devx_rx.c) or, with udp=True, IPv4/UDP {dst ip, dst port,
                           !frag} plus an optional flow TAG (devx_rx_s0.c)

Three CQ umem fields are `reserved` in OFED's mlx5_ifc.h; their positions are the ones DPDK's mlx5_prm.h names
(cqc.dbr_umem_id = cqc reserved_at_20; create_cq_in cq_umem_offset / cq_umem_id = reserved_at_280[0x60]), set through
the reserved names exactly as the C helpers do. Each function returns a dict of the object handles and numbers, or
raises DevxError with the C helpers' message (command, errno, status, syndrome).
"""


class DevxError(RuntimeError):
    pass


def _err(V, L, what, out):
    return DevxError('%s: errno %d status 0x%x syndrome 0x%x' % (what, V.errno(), L.get(out, 'alloc_transport_domain_out', 'status'),
                                                                 L.get(out, 'alloc_transport_domain_out', 'syndrome')))


def _create(V, L, ctx, inb, outtyp, what):
    h, out = V.obj_create(ctx, inb, L.size(outtyp))
    if not h:
        raise _err(V, L, what, out)
    return h, out


def _cq_in(L, uar_page, umem_id, cq_off, dbr_off, log_cq, eqn):
    E = L.enum
    b = L.buf('create_cq_in')
    c = L.off('create_cq_in', 'cq_context')
    L.set(b, 'create_cq_in', 'opcode', E('MLX5_CMD_OP_CREATE_CQ'))
    L.set(b, 'cqc', 'cqe_sz', 0, c)                     # 64-byte CQEs
    L.set(b, 'cqc', 'log_cq_size', log_cq, c)
    L.set(b, 'cqc', 'uar_page', uar_page, c)
    L.set(b, 'cqc', 'c_eqn_or_apu_element', eqn, c)
    L.set(b, 'cqc', 'log_page_size', 0, c)              # 4 KB
    L.set(b, 'cqc', 'dbr_umem_valid', 1, c)
    L.set(b, 'cqc', 'reserved_at_20', umem_id, c)       # dbr_umem_id
    L.set64(b, 'cqc', 'dbr_addr', dbr_off, c)
    o, w = L.field('create_cq_in', 'reserved_at_280')   # cq_umem_offset (64) + cq_umem_id (32)
    assert w == 0x60, 'create_cq_in.reserved_at_280 is not 0x60 bits wide in this mlx5_ifc.h'
    d = o // 8
    b[d:d + 12] = cq_off.to_bytes(8, 'big') + umem_id.to_bytes(4, 'big')
    L.set(b, 'create_cq_in', 'cq_umem_valid', 1)
    return b


def cq_create(V, L, ctx, uar_page, umem_id, cq_off, dbr_off, log_cq):
    """e3_cq_create -> {'cq', 'cqn', 'eqn'}"""
    eqn = V.query_eqn(ctx, 0)
    h, out = _create(V, L, ctx, _cq_in(L, uar_page, umem_id, cq_off, dbr_off, log_cq, eqn), 'create_cq_out', 'CREATE_CQ')
    return {'cq': h, 'cqn': L.get(out, 'create_cq_out', 'cqn'), 'eqn': eqn}


def query_min_inline(V, L, ctx):
    """e1_query_min_inline: min_wqe_inline_mode of this function's NIC vport (0 none, 1 L2, 2 IP, 3 TCP/UDP)."""
    b = L.buf('query_nic_vport_context_in')
    L.set(b, 'query_nic_vport_context_in', 'opcode', L.enum('MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT'))
    rc, out = V.general_cmd(ctx, b, L.size('query_nic_vport_context_out'))
    if rc:
        raise _err(V, L, 'QUERY_NIC_VPORT_CONTEXT', out)
    return L.get(out, 'query_nic_vport_context_out', 'nic_vport_context.min_wqe_inline_mode')


def query_hca_freq_khz(V, L, ctx):
    """The ConnectX core clock (cmd_hca_cap.device_frequency_khz; what ibv_query_device_ex reports as hca_core_clock),
    for converting CQE timestamps."""
    b = L.buf('query_hca_cap_in')
    L.set(b, 'query_hca_cap_in', 'opcode', L.enum('MLX5_CMD_OP_QUERY_HCA_CAP'))
    L.set(b, 'query_hca_cap_in', 'op_mod', (L.enum('MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE') << 1) | 1)   # current values
    rc, out = V.general_cmd(ctx, b, L.size('query_hca_cap_out'))
    if rc:
        raise _err(V, L, 'QUERY_HCA_CAP', out)
    return L.get(out, 'query_hca_cap_out', 'capability.cmd_hca_cap.device_frequency_khz')


def _td(V, L, ctx):
    b = L.buf('alloc_transport_domain_in')
    L.set(b, 'alloc_transport_domain_in', 'opcode', L.enum('MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN'))
    h, out = _create(V, L, ctx, b, 'alloc_transport_domain_out', 'ALLOC_TRANSPORT_DOMAIN')
    return h, L.get(out, 'alloc_transport_domain_out', 'transport_domain')


def sq_create(V, L, ctx, pdn, cqn, uar_page, wq_umem_id, wq_off, dbr_umem_id, dbr_off, log_wq_sz, min_inline):
    """e1_create_sq -> {'td', 'tdn', 'tis', 'tisn', 'sq', 'sqn'}"""
    E, s = L.enum, {}
    s['td'], s['tdn'] = _td(V, L, ctx)
    b = L.buf('create_tis_in')
    L.set(b, 'create_tis_in', 'opcode', E('MLX5_CMD_OP_CREATE_TIS'))
    L.set(b, 'create_tis_in', 'ctx.transport_domain', s['tdn'])
    s['tis'], out = _create(V, L, ctx, b, 'create_tis_out', 'CREATE_TIS')
    s['tisn'] = L.get(out, 'create_tis_out', 'tisn')
    b = L.buf('create_sq_in')
    q, w = L.off('create_sq_in', 'ctx'), L.off('create_sq_in', 'ctx.wq')
    L.set(b, 'create_sq_in', 'opcode', E('MLX5_CMD_OP_CREATE_SQ'))
    L.set(b, 'sqc', 'flush_in_error_en', 1, q)
    L.set(b, 'sqc', 'min_wqe_inline_mode', min_inline, q)
    L.set(b, 'sqc', 'state', E('MLX5_SQC_STATE_RST'), q)
    L.set(b, 'sqc', 'cqn', cqn, q)
    L.set(b, 'sqc', 'tis_lst_sz', 1, q)
    L.set(b, 'sqc', 'tis_num_0', s['tisn'], q)
    L.set(b, 'wq', 'wq_type', E('MLX5_WQ_TYPE_CYCLIC'), w)
    L.set(b, 'wq', 'pd', pdn, w)
    L.set(b, 'wq', 'uar_page', uar_page, w)
    L.set64(b, 'wq', 'dbr_addr', dbr_off, w)
    L.set(b, 'wq', 'log_wq_stride', 6, w)               # 64-byte WQEBB
    L.set(b, 'wq', 'log_wq_pg_sz', 0, w)                # 4 KB pages
    L.set(b, 'wq', 'log_wq_sz', log_wq_sz, w)
    L.set(b, 'wq', 'dbr_umem_valid', 1, w)
    L.set(b, 'wq', 'wq_umem_valid', 1, w)
    L.set(b, 'wq', 'dbr_umem_id', dbr_umem_id, w)
    L.set(b, 'wq', 'wq_umem_id', wq_umem_id, w)
    L.set64(b, 'wq', 'wq_umem_offset', wq_off, w)
    s['sq'], out = _create(V, L, ctx, b, 'create_sq_out', 'CREATE_SQ')
    s['sqn'] = L.get(out, 'create_sq_out', 'sqn')
    b = L.buf('modify_sq_in')
    q = L.off('modify_sq_in', 'ctx')
    L.set(b, 'modify_sq_in', 'opcode', E('MLX5_CMD_OP_MODIFY_SQ'))
    L.set(b, 'modify_sq_in', 'sq_state', E('MLX5_SQC_STATE_RST'))
    L.set(b, 'modify_sq_in', 'sqn', s['sqn'])
    L.set(b, 'sqc', 'state', E('MLX5_SQC_STATE_RDY'), q)
    rc, out = V.obj_modify(s['sq'], b, L.size('modify_sq_out'))
    if rc:
        raise _err(V, L, 'MODIFY_SQ RST->RDY', out)
    return s


def rx_create(V, L, ctx, pdn, uar_page, umem_id, cq_off, cq_dbr_off, rq_off, rq_dbr_off, log_cq, log_rq, ethertype,
              udp=False, dst_ip=0, udp_dport=0, tag=0):
    """e2_rx_create -> {'td', 'tdn', 'cq', 'cqn', 'eqn', 'rq', 'rqn', 'tir', 'tirn', 'matcher', 'flow'}"""
    E, r = L.enum, {}
    r['eqn'] = V.query_eqn(ctx, 0)
    r['td'], r['tdn'] = _td(V, L, ctx)
    r['cq'], out = _create(V, L, ctx, _cq_in(L, uar_page, umem_id, cq_off, cq_dbr_off, log_cq, r['eqn']), 'create_cq_out', 'CREATE_CQ')
    r['cqn'] = L.get(out, 'create_cq_out', 'cqn')
    b = L.buf('create_rq_in')
    q, w = L.off('create_rq_in', 'ctx'), L.off('create_rq_in', 'ctx.wq')
    L.set(b, 'create_rq_in', 'opcode', E('MLX5_CMD_OP_CREATE_RQ'))
    L.set(b, 'rqc', 'vsd', 1, q)                        # no VLAN stripping
    L.set(b, 'rqc', 'mem_rq_type', E('MLX5_RQC_MEM_RQ_TYPE_MEMORY_RQ_INLINE'), q)
    L.set(b, 'rqc', 'state', E('MLX5_RQC_STATE_RST'), q)
    L.set(b, 'rqc', 'flush_in_error_en', 1, q)
    L.set(b, 'rqc', 'cqn', r['cqn'], q)
    L.set(b, 'wq', 'wq_type', E('MLX5_WQ_TYPE_CYCLIC'), w)
    L.set(b, 'wq', 'pd', pdn, w)
    L.set64(b, 'wq', 'dbr_addr', rq_dbr_off, w)
    L.set(b, 'wq', 'log_wq_stride', 4, w)               # 16 B: one data segment
    L.set(b, 'wq', 'log_wq_pg_sz', 0, w)
    L.set(b, 'wq', 'log_wq_sz', log_rq, w)
    L.set(b, 'wq', 'dbr_umem_valid', 1, w)
    L.set(b, 'wq', 'wq_umem_valid', 1, w)
    L.set(b, 'wq', 'dbr_umem_id', umem_id, w)
    L.set(b, 'wq', 'wq_umem_id', umem_id, w)
    L.set64(b, 'wq', 'wq_umem_offset', rq_off, w)
    r['rq'], out = _create(V, L, ctx, b, 'create_rq_out', 'CREATE_RQ')
    r['rqn'] = L.get(out, 'create_rq_out', 'rqn')
    b = L.buf('modify_rq_in')
    q = L.off('modify_rq_in', 'ctx')
    L.set(b, 'modify_rq_in', 'opcode', E('MLX5_CMD_OP_MODIFY_RQ'))
    L.set(b, 'modify_rq_in', 'rq_state', E('MLX5_RQC_STATE_RST'))
    L.set(b, 'modify_rq_in', 'rqn', r['rqn'])
    L.set(b, 'rqc', 'state', E('MLX5_RQC_STATE_RDY'), q)
    rc, out = V.obj_modify(r['rq'], b, L.size('modify_rq_out'))
    if rc:
        raise _err(V, L, 'MODIFY_RQ RST->RDY', out)
    b = L.buf('create_tir_in')
    q = L.off('create_tir_in', 'ctx')
    L.set(b, 'create_tir_in', 'opcode', E('MLX5_CMD_OP_CREATE_TIR'))
    L.set(b, 'tirc', 'disp_type', E('MLX5_TIRC_DISP_TYPE_DIRECT'), q)
    L.set(b, 'tirc', 'inline_rqn', r['rqn'], q)
    L.set(b, 'tirc', 'transport_domain', r['tdn'], q)
    r['tir'], out = _create(V, L, ctx, b, 'create_tir_out', 'CREATE_TIR')
    r['tirn'] = L.get(out, 'create_tir_out', 'tirn')
    mask, val = L.buf('fte_match_param'), L.buf('fte_match_param')
    F = 'fte_match_param'
    L.set(mask, F, 'outer_headers.ethertype', 0xffff)
    L.set(val, F, 'outer_headers.ethertype', ethertype)
    if udp:
        L.set(mask, F, 'outer_headers.ip_protocol', 0xff)
        L.set(val, F, 'outer_headers.ip_protocol', 17)
        L.set(mask, F, 'outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4', 0xffffffff)
        L.set(val, F, 'outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4', dst_ip)
        L.set(mask, F, 'outer_headers.udp_dport', 0xffff)
        L.set(val, F, 'outer_headers.udp_dport', udp_dport)
        L.set(mask, F, 'outer_headers.frag', 1)
        L.set(val, F, 'outer_headers.frag', 0)
    import verbs
    acts = [(verbs.MLX5DV_FLOW_ACTION_DEST_DEVX, r['tir'])]
    if tag:
        acts.append((verbs.MLX5DV_FLOW_ACTION_TAG, tag))
    r['matcher'], r['flow'], e = V.flow(ctx, mask, val, acts)
    if e:
        raise DevxError(e)
    return r
