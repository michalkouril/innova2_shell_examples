/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 * DevX raw-Ethernet send queue whose WQE ring and doorbell record live in caller-supplied umems (E1). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#ifdef __cplusplus
extern "C" {
#endif
struct e1_sq {
	struct mlx5dv_devx_obj *td, *tis, *sq;
	uint32_t tdn, tisn, sqn;
};
/* min_wqe_inline_mode of this function's NIC vport (0 none, 1 L2, 2 IP, 3 TCP/UDP); <0 on error */
int e1_query_min_inline(struct ibv_context *ctx, char *err, size_t errlen);
int e1_create_sq(struct ibv_context *ctx, uint32_t pdn, uint32_t cqn, uint32_t uar_page,
		 uint32_t wq_umem_id, uint64_t wq_off, uint32_t dbr_umem_id, uint64_t dbr_off,
		 int log_wq_sz, int min_inline, struct e1_sq *sq, char *err, size_t errlen);
void e1_destroy_sq(struct e1_sq *sq);
#ifdef __cplusplus
}
#endif
