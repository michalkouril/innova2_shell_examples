/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#ifdef __cplusplus
extern "C" {
#endif
struct e3_cq {
	struct mlx5dv_devx_obj *cq;
	uint32_t cqn, eqn;
};
int e3_cq_create(struct ibv_context *ctx, uint32_t uar_page, uint32_t umem_id, uint64_t cq_off, uint64_t dbr_off,
		 int log_cq, struct e3_cq *q, char *err, size_t errlen);
void e3_cq_destroy(struct e3_cq *q);
#ifdef __cplusplus
}
#endif
