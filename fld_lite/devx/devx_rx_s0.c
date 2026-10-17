/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * E2 DevX objects: transport domain, CQ (ring + doorbell record in a umem), cyclic RQ (ring + doorbell record in the
 * same umem) RST->RDY, a direct TIR, and a NIC-RX flow rule steering one EtherType to that TIR.
 *
 * Three CQ umem fields are `reserved` in the OFED mlx5_ifc.h used here; their positions are those DPDK's mlx5_prm.h
 * names (cqc.dbr_umem_id at cqc bit 0x20; create_cq_in.cq_umem_offset at 0x280, cq_umem_id at 0x2c0). They are set
 * through the reserved names so the compiler still checks the widths.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "prm.h"
#include "devx_rx_s0.h"

static int cmd_err(const char *what, const void *out, char *err, size_t errlen)
{
	snprintf(err, errlen, "%s: errno %d status 0x%x syndrome 0x%x", what, errno,
		 MLX5_GET(alloc_transport_domain_out, out, status), MLX5_GET(alloc_transport_domain_out, out, syndrome));
	return -1;
}

int e2_rx_create(struct ibv_context *ctx, const struct e2_rx_cfg *c, struct e2_rx *r, char *err, size_t errlen)
{
	memset(r, 0, sizeof(*r));
	if (mlx5dv_devx_query_eqn(ctx, 0, &r->eqn)) {
		snprintf(err, errlen, "query_eqn: errno %d", errno);
		return -1;
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(alloc_transport_domain_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(alloc_transport_domain_out)] = {0};

		MLX5_SET(alloc_transport_domain_in, in, opcode, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
		r->td = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!r->td)
			return cmd_err("ALLOC_TRANSPORT_DOMAIN", out, err, errlen);
		r->tdn = MLX5_GET(alloc_transport_domain_out, out, transport_domain);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_cq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_cq_out)] = {0};
		void *cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);

		MLX5_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		MLX5_SET(cqc, cqc, cqe_sz, 0);                  /* 64-byte CQEs */
		MLX5_SET(cqc, cqc, log_cq_size, c->log_cq);
		MLX5_SET(cqc, cqc, uar_page, c->uar_page);
		MLX5_SET(cqc, cqc, c_eqn_or_apu_element, r->eqn);
		MLX5_SET(cqc, cqc, log_page_size, 0);           /* 4 KB */
		MLX5_SET(cqc, cqc, dbr_umem_valid, 1);
		MLX5_SET(cqc, cqc, reserved_at_20, c->umem_id);  /* dbr_umem_id */
		MLX5_SET64(cqc, cqc, dbr_addr, c->cq_dbr_off);
		/* cq_umem_offset (64) + cq_umem_id (32) = reserved_at_280[0x60] */
		{
			uint32_t *d = (uint32_t *)MLX5_ADDR_OF(create_cq_in, in, reserved_at_280);

			_Static_assert(__mlx5_bit_sz(create_cq_in, reserved_at_280) == 0x60, "layout");
			d[0] = htobe32((uint32_t)(c->cq_off >> 32));
			d[1] = htobe32((uint32_t)c->cq_off);
			d[2] = htobe32(c->umem_id);
		}
		MLX5_SET(create_cq_in, in, cq_umem_valid, 1);
		r->cq = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!r->cq)
			return cmd_err("CREATE_CQ", out, err, errlen);
		r->cqn = MLX5_GET(create_cq_out, out, cqn);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_rq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_rq_out)] = {0};
		void *rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
		void *wq = MLX5_ADDR_OF(rqc, rqc, wq);

		MLX5_SET(create_rq_in, in, opcode, MLX5_CMD_OP_CREATE_RQ);
		MLX5_SET(rqc, rqc, vsd, 1);                      /* no VLAN stripping */
		MLX5_SET(rqc, rqc, mem_rq_type, MLX5_RQC_MEM_RQ_TYPE_MEMORY_RQ_INLINE);
		MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RST);
		MLX5_SET(rqc, rqc, flush_in_error_en, 1);
		MLX5_SET(rqc, rqc, cqn, r->cqn);
		MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
		MLX5_SET(wq, wq, pd, c->pdn);
		MLX5_SET64(wq, wq, dbr_addr, c->rq_dbr_off);
		MLX5_SET(wq, wq, log_wq_stride, 4);              /* 16 B: one data segment */
		MLX5_SET(wq, wq, log_wq_pg_sz, 0);
		MLX5_SET(wq, wq, log_wq_sz, c->log_rq);
		MLX5_SET(wq, wq, dbr_umem_valid, 1);
		MLX5_SET(wq, wq, wq_umem_valid, 1);
		MLX5_SET(wq, wq, dbr_umem_id, c->umem_id);
		MLX5_SET(wq, wq, wq_umem_id, c->umem_id);
		MLX5_SET64(wq, wq, wq_umem_offset, c->rq_off);
		r->rq = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!r->rq)
			return cmd_err("CREATE_RQ", out, err, errlen);
		r->rqn = MLX5_GET(create_rq_out, out, rqn);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(modify_rq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(modify_rq_out)] = {0};
		void *rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

		MLX5_SET(modify_rq_in, in, opcode, MLX5_CMD_OP_MODIFY_RQ);
		MLX5_SET(modify_rq_in, in, rq_state, MLX5_RQC_STATE_RST);
		MLX5_SET(modify_rq_in, in, rqn, r->rqn);
		MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RDY);
		if (mlx5dv_devx_obj_modify(r->rq, in, sizeof(in), out, sizeof(out)))
			return cmd_err("MODIFY_RQ RST->RDY", out, err, errlen);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_tir_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_tir_out)] = {0};
		void *tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);

		MLX5_SET(create_tir_in, in, opcode, MLX5_CMD_OP_CREATE_TIR);
		MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_DIRECT);
		MLX5_SET(tirc, tirc, inline_rqn, r->rqn);
		MLX5_SET(tirc, tirc, transport_domain, r->tdn);
		r->tir = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!r->tir)
			return cmd_err("CREATE_TIR", out, err, errlen);
		r->tirn = MLX5_GET(create_tir_out, out, tirn);
	}
	{
		static uint8_t mbuf[sizeof(struct mlx5dv_flow_match_parameters) + MLX5_ST_SZ_BYTES(fte_match_param)];
		static uint8_t vbuf[sizeof(struct mlx5dv_flow_match_parameters) + MLX5_ST_SZ_BYTES(fte_match_param)];
		struct mlx5dv_flow_match_parameters *mask = (void *)mbuf, *val = (void *)vbuf;
		struct mlx5dv_flow_matcher_attr ma = {0};
		struct mlx5dv_flow_action_attr act[2];
		int nact = 1;

		memset(mbuf, 0, sizeof(mbuf));
		memset(vbuf, 0, sizeof(vbuf));
		mask->match_sz = val->match_sz = MLX5_ST_SZ_BYTES(fte_match_param);
		MLX5_SET(fte_match_param, mask->match_buf, outer_headers.ethertype, 0xffff);
		MLX5_SET(fte_match_param, val->match_buf, outer_headers.ethertype, c->ethertype);
		if (c->udp) {
			MLX5_SET(fte_match_param, mask->match_buf, outer_headers.ip_protocol, 0xff);
			MLX5_SET(fte_match_param, val->match_buf, outer_headers.ip_protocol, 17);
			MLX5_SET(fte_match_param, mask->match_buf, outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4, 0xffffffff);
			MLX5_SET(fte_match_param, val->match_buf, outer_headers.dst_ipv4_dst_ipv6.ipv4_layout.ipv4, c->dst_ip);
			MLX5_SET(fte_match_param, mask->match_buf, outer_headers.udp_dport, 0xffff);
			MLX5_SET(fte_match_param, val->match_buf, outer_headers.udp_dport, c->udp_dport);
			MLX5_SET(fte_match_param, mask->match_buf, outer_headers.frag, 1);
			MLX5_SET(fte_match_param, val->match_buf, outer_headers.frag, 0);
		}
		ma.type = IBV_FLOW_ATTR_NORMAL;
		ma.priority = 0;
		ma.match_criteria_enable = 1 << 0;               /* outer headers */
		ma.match_mask = mask;
		ma.comp_mask = MLX5DV_FLOW_MATCHER_MASK_FT_TYPE;
		ma.ft_type = MLX5DV_FLOW_TABLE_TYPE_NIC_RX;
		r->matcher = mlx5dv_create_flow_matcher(ctx, &ma);
		if (!r->matcher) {
			snprintf(err, errlen, "create_flow_matcher: errno %d", errno);
			return -1;
		}
		memset(act, 0, sizeof(act));
		act[0].type = MLX5DV_FLOW_ACTION_DEST_DEVX;
		act[0].obj = r->tir;
		if (c->tag) { act[1].type = MLX5DV_FLOW_ACTION_TAG; act[1].tag_value = c->tag; nact = 2; }
		r->flow = mlx5dv_create_flow(r->matcher, val, nact, act);
		if (!r->flow) {
			snprintf(err, errlen, "create_flow: errno %d", errno);
			return -1;
		}
	}
	return 0;
}

void e2_rx_destroy(struct e2_rx *r)
{
	if (r->flow)
		ibv_destroy_flow(r->flow);
	if (r->matcher)
		mlx5dv_destroy_flow_matcher(r->matcher);
	if (r->tir)
		mlx5dv_devx_obj_destroy(r->tir);
	if (r->rq)
		mlx5dv_devx_obj_destroy(r->rq);
	if (r->cq)
		mlx5dv_devx_obj_destroy(r->cq);
	if (r->td)
		mlx5dv_devx_obj_destroy(r->td);
	memset(r, 0, sizeof(*r));
}
