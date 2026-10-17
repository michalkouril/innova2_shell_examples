/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 * DevX CQ whose ring and doorbell record live in a caller umem (same layout rules as e2/devx_rx.c: the CQ umem fields
 * are `reserved` in the OFED mlx5_ifc.h; positions per DPDK's mlx5_prm.h, proven on hardware in E2). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "prm.h"
#include "devx_cq.h"

int e3_cq_create(struct ibv_context *ctx, uint32_t uar_page, uint32_t umem_id, uint64_t cq_off, uint64_t dbr_off,
		 int log_cq, struct e3_cq *q, char *err, size_t errlen)
{
	uint32_t in[MLX5_ST_SZ_DW(create_cq_in)] = {0};
	uint32_t out[MLX5_ST_SZ_DW(create_cq_out)] = {0};
	void *cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);
	uint32_t *d = (uint32_t *)MLX5_ADDR_OF(create_cq_in, in, reserved_at_280);

	memset(q, 0, sizeof(*q));
	if (mlx5dv_devx_query_eqn(ctx, 0, &q->eqn)) {
		snprintf(err, errlen, "query_eqn: errno %d", errno);
		return -1;
	}
	MLX5_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
	MLX5_SET(cqc, cqc, cqe_sz, 0);
	MLX5_SET(cqc, cqc, log_cq_size, log_cq);
	MLX5_SET(cqc, cqc, uar_page, uar_page);
	MLX5_SET(cqc, cqc, c_eqn_or_apu_element, q->eqn);
	MLX5_SET(cqc, cqc, log_page_size, 0);
	MLX5_SET(cqc, cqc, dbr_umem_valid, 1);
	MLX5_SET(cqc, cqc, reserved_at_20, umem_id);            /* dbr_umem_id */
	MLX5_SET64(cqc, cqc, dbr_addr, dbr_off);
	d[0] = htobe32((uint32_t)(cq_off >> 32));               /* cq_umem_offset */
	d[1] = htobe32((uint32_t)cq_off);
	d[2] = htobe32(umem_id);                                /* cq_umem_id */
	MLX5_SET(create_cq_in, in, cq_umem_valid, 1);
	q->cq = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!q->cq) {
		snprintf(err, errlen, "CREATE_CQ: errno %d status 0x%x syndrome 0x%x", errno,
			 MLX5_GET(create_cq_out, out, status), MLX5_GET(create_cq_out, out, syndrome));
		return -1;
	}
	q->cqn = MLX5_GET(create_cq_out, out, cqn);
	return 0;
}

void e3_cq_destroy(struct e3_cq *q)
{
	if (q->cq)
		mlx5dv_devx_obj_destroy(q->cq);
	memset(q, 0, sizeof(*q));
}
