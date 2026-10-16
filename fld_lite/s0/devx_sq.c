/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * E1 DevX objects: transport domain -> TIS -> SQ (RST -> RDY). The SQ's WQ buffer and doorbell record are umems the
 * caller registered -- on the FPGA side, an XRT P2P buffer object mapped through BAR4 and made registrable by the
 * innova2_peermem peer-memory client. Layouts come from mlx5_ifc.h via prm.h.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "prm.h"
#include "devx_sq.h"

static int cmd_err(const char *what, const void *out, char *err, size_t errlen)
{
	snprintf(err, errlen, "%s: errno %d status 0x%x syndrome 0x%x", what, errno,
		 MLX5_GET(alloc_transport_domain_out, out, status), MLX5_GET(alloc_transport_domain_out, out, syndrome));
	return -1;
}

int e1_query_min_inline(struct ibv_context *ctx, char *err, size_t errlen)
{
	uint32_t in[MLX5_ST_SZ_DW(query_nic_vport_context_in)] = {0};
	uint32_t out[MLX5_ST_SZ_DW(query_nic_vport_context_out)] = {0};

	MLX5_SET(query_nic_vport_context_in, in, opcode, MLX5_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
	if (mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out)))
		return cmd_err("QUERY_NIC_VPORT_CONTEXT", out, err, errlen);
	return MLX5_GET(query_nic_vport_context_out, out, nic_vport_context.min_wqe_inline_mode);
}

int e1_create_sq(struct ibv_context *ctx, uint32_t pdn, uint32_t cqn, uint32_t uar_page,
		 uint32_t wq_umem_id, uint64_t wq_off, uint32_t dbr_umem_id, uint64_t dbr_off,
		 int log_wq_sz, int min_inline, struct e1_sq *s, char *err, size_t errlen)
{
	memset(s, 0, sizeof(*s));
	{
		uint32_t in[MLX5_ST_SZ_DW(alloc_transport_domain_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(alloc_transport_domain_out)] = {0};

		MLX5_SET(alloc_transport_domain_in, in, opcode, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
		s->td = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!s->td)
			return cmd_err("ALLOC_TRANSPORT_DOMAIN", out, err, errlen);
		s->tdn = MLX5_GET(alloc_transport_domain_out, out, transport_domain);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_tis_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_tis_out)] = {0};

		MLX5_SET(create_tis_in, in, opcode, MLX5_CMD_OP_CREATE_TIS);
		MLX5_SET(create_tis_in, in, ctx.transport_domain, s->tdn);
		s->tis = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!s->tis)
			return cmd_err("CREATE_TIS", out, err, errlen);
		s->tisn = MLX5_GET(create_tis_out, out, tisn);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(create_sq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(create_sq_out)] = {0};
		void *sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
		void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

		MLX5_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);
		MLX5_SET(sqc, sqc, flush_in_error_en, 1);
		MLX5_SET(sqc, sqc, min_wqe_inline_mode, min_inline);
		MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RST);
		MLX5_SET(sqc, sqc, cqn, cqn);
		MLX5_SET(sqc, sqc, tis_lst_sz, 1);
		MLX5_SET(sqc, sqc, tis_num_0, s->tisn);
		MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
		MLX5_SET(wq, wq, pd, pdn);
		MLX5_SET(wq, wq, uar_page, uar_page);
		MLX5_SET64(wq, wq, dbr_addr, dbr_off);
		MLX5_SET(wq, wq, log_wq_stride, 6);             /* 64-byte WQEBB */
		MLX5_SET(wq, wq, log_wq_pg_sz, 0);              /* 4 KB pages */
		MLX5_SET(wq, wq, log_wq_sz, log_wq_sz);
		MLX5_SET(wq, wq, dbr_umem_valid, 1);
		MLX5_SET(wq, wq, wq_umem_valid, 1);
		MLX5_SET(wq, wq, dbr_umem_id, dbr_umem_id);
		MLX5_SET(wq, wq, wq_umem_id, wq_umem_id);
		MLX5_SET64(wq, wq, wq_umem_offset, wq_off);
		s->sq = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
		if (!s->sq)
			return cmd_err("CREATE_SQ", out, err, errlen);
		s->sqn = MLX5_GET(create_sq_out, out, sqn);
	}
	{
		uint32_t in[MLX5_ST_SZ_DW(modify_sq_in)] = {0};
		uint32_t out[MLX5_ST_SZ_DW(modify_sq_out)] = {0};
		void *sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);

		MLX5_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
		MLX5_SET(modify_sq_in, in, sq_state, MLX5_SQC_STATE_RST);
		MLX5_SET(modify_sq_in, in, sqn, s->sqn);
		MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RDY);
		if (mlx5dv_devx_obj_modify(s->sq, in, sizeof(in), out, sizeof(out)))
			return cmd_err("MODIFY_SQ RST->RDY", out, err, errlen);
	}
	return 0;
}

void e1_destroy_sq(struct e1_sq *s)
{
	if (s->sq)
		mlx5dv_devx_obj_destroy(s->sq);
	if (s->tis)
		mlx5dv_devx_obj_destroy(s->tis);
	if (s->td)
		mlx5dv_devx_obj_destroy(s->td);
	memset(s, 0, sizeof(*s));
}
