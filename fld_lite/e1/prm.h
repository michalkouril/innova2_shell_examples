/* SPDX-FileCopyrightText: 2026 the innova2 contributors
 * SPDX-FileCopyrightText: Mellanox Technologies Ltd.
 * SPDX-License-Identifier: Apache-2.0 AND Linux-OpenIB
 *
 * The MLX5_* / __mlx5_* accessor macros below follow include/linux/mlx5/device.h (Mellanox, dual GPL-2.0 / OpenIB.org
 * BSD), rewritten for user space; used under the OpenIB.org BSD licence (LICENSES/Linux-OpenIB.txt).
 *
 * Userspace shim for the kernel/OFED mlx5_ifc.h (PRM layouts as u8 bit-arrays), so DevX command layouts come from the
 * header rather than hand-counted offsets. mlx5_ifc.h / mlx5_ifc_fpga.h are copied from the running OFED tree
 * (/usr/src/ofa_kernel/x86_64/<kver>/include/linux/mlx5/, dual GPL-2.0 / OpenIB BSD) at build time, not committed.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <endian.h>
typedef uint8_t u8;
#define __packed __attribute__((packed))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define DECLARE_FLEX_ARRAY(T, name) struct { struct { } __empty_##name; T name[]; }
#include "mlx5_ifc.h"

#define __mlx5_nullp(typ) ((struct mlx5_ifc_##typ##_bits *)0)
#define __mlx5_bit_sz(typ, fld) sizeof(__mlx5_nullp(typ)->fld)
#define __mlx5_bit_off(typ, fld) (offsetof(struct mlx5_ifc_##typ##_bits, fld))
#define __mlx5_dw_off(typ, fld) (__mlx5_bit_off(typ, fld) / 32)
#define __mlx5_dw_bit_off(typ, fld) (32 - __mlx5_bit_sz(typ, fld) - (__mlx5_bit_off(typ, fld) & 0x1f))
#define __mlx5_mask(typ, fld) ((uint32_t)((1ull << __mlx5_bit_sz(typ, fld)) - 1))
#define __mlx5_dw_mask(typ, fld) (__mlx5_mask(typ, fld) << __mlx5_dw_bit_off(typ, fld))
#define MLX5_ST_SZ_BYTES(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 8)
#define MLX5_ST_SZ_DW(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 32)
#define MLX5_ADDR_OF(typ, p, fld) ((void *)((uint8_t *)(p) + (__mlx5_bit_off(typ, fld) / 8)))
#define MLX5_SET(typ, p, fld, v) do { \
	uint32_t *_d = (uint32_t *)(p) + __mlx5_dw_off(typ, fld); \
	*_d = htobe32((be32toh(*_d) & ~__mlx5_dw_mask(typ, fld)) | \
		      (((uint32_t)(v) & __mlx5_mask(typ, fld)) << __mlx5_dw_bit_off(typ, fld))); \
} while (0)
#define MLX5_SET64(typ, p, fld, v) do { \
	_Static_assert(__mlx5_bit_sz(typ, fld) == 64, "not 64-bit"); \
	*(uint64_t *)MLX5_ADDR_OF(typ, p, fld) = htobe64(v); \
} while (0)
#define MLX5_GET(typ, p, fld) \
	((be32toh(*((const uint32_t *)(p) + __mlx5_dw_off(typ, fld))) >> __mlx5_dw_bit_off(typ, fld)) & \
	 __mlx5_mask(typ, fld))
