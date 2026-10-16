/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-License-Identifier: Apache-2.0
 * E2 DevX receive path: CQ + cyclic RQ whose rings and doorbell records live in caller umems, a direct TIR, and a
 * NIC-RX steering rule to it.
 * s0 variant (UDP architecture step 0): the rule matches IPv4/UDP {dst ip, dst port, frag} and adds a flow TAG. */
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
	int udp;                        /* 1: match ethertype 0x0800, ip_protocol 17, dst_ip, udp_dport, frag = 0 */
	uint32_t dst_ip;                /* host order */
	uint16_t udp_dport;
	uint32_t tag;                   /* 0: no TAG action */
};
int e2_rx_create(struct ibv_context *ctx, const struct e2_rx_cfg *cfg, struct e2_rx *rx, char *err, size_t errlen);
void e2_rx_destroy(struct e2_rx *rx);
#ifdef __cplusplus
}
#endif
