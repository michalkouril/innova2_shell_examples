/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 * E2 DevX receive path: CQ + cyclic RQ whose rings and doorbell records live in caller umems, a direct TIR, and a
 * NIC-RX steering rule (EtherType) to it. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#ifdef __cplusplus
extern "C" {
#endif
struct e2_rx {
	struct mlx5dv_devx_obj *td, *cq, *rq, *tir;
	struct mlx5dv_flow_matcher *matcher;
	struct ibv_flow *flow;
	uint32_t tdn, cqn, rqn, tirn, eqn;
};
struct e2_rx_cfg {
	uint32_t pdn, uar_page;
	uint32_t umem_id;               /* one umem holding all four structures */
	uint64_t cq_off, cq_dbr_off, rq_off, rq_dbr_off;   /* offsets inside that umem */
	int log_cq, log_rq;             /* CQ: 64-byte CQEs; RQ: 16-byte WQEs (one data segment) */
	uint16_t ethertype;
};
int e2_rx_create(struct ibv_context *ctx, const struct e2_rx_cfg *cfg, struct e2_rx *rx, char *err, size_t errlen);
void e2_rx_destroy(struct e2_rx *rx);
#ifdef __cplusplus
}
#endif
