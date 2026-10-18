# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""verbs -- the libibverbs / libmlx5 (DevX) calls the FLD-lite hosts make, through ctypes on the installed libraries.

Only exported library functions are called: the inline helpers of verbs.h / mlx5dv.h that the C hosts use are
re-done here (mlx5dv_set_data_seg; mlx5dv_create_flow -> __mlx5dv_create_flow). The structures are the rdma-core
ABI (verbs.h, mlx5dv.h): ibv_mr, mlx5dv_devx_umem, mlx5dv_devx_uar, mlx5dv_obj, mlx5dv_pd, mlx5dv_context_attr,
mlx5dv_flow_matcher_attr, mlx5dv_flow_match_parameters, mlx5dv_flow_action_attr. Constants that are kernel UAPI
(rdma/mlx5_user_ioctl_verbs.h) or rdma-core enums are listed below with their header.

Objects are not destroyed by the hosts (as the C e5udp_host): closing the process releases every verbs / DevX object.
"""
import ctypes as C
import ctypes.util

import numpy as np

IBV_ACCESS_LOCAL_WRITE = 1                       # verbs.h enum ibv_access_flags
IBV_FLOW_ATTR_NORMAL = 0                         # verbs.h enum ibv_flow_attr_type
MLX5DV_CONTEXT_FLAGS_DEVX = 1 << 0               # mlx5dv.h enum mlx5dv_context_attr_flags
MLX5DV_OBJ_PD = 1 << 6                           # mlx5dv.h enum mlx5dv_obj_type
MLX5DV_FLOW_MATCHER_MASK_FT_TYPE = 1 << 0        # mlx5dv.h enum mlx5dv_flow_matcher_attr_mask
MLX5DV_FLOW_TABLE_TYPE_NIC_RX = 0                # rdma/mlx5_user_ioctl_verbs.h MLX5_IB_UAPI_FLOW_TABLE_TYPE_NIC_RX
MLX5DV_UAR_ALLOC_TYPE_NC = 1                     # rdma/mlx5_user_ioctl_verbs.h MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC
MLX5DV_FLOW_ACTION_TAG = 4                       # mlx5dv.h enum mlx5dv_flow_action_type
MLX5DV_FLOW_ACTION_DEST_DEVX = 5


class ibv_mr(C.Structure):
    _fields_ = [('context', C.c_void_p), ('pd', C.c_void_p), ('addr', C.c_void_p), ('length', C.c_size_t),
                ('handle', C.c_uint32), ('lkey', C.c_uint32), ('rkey', C.c_uint32)]


class devx_umem(C.Structure):
    _fields_ = [('umem_id', C.c_uint32)]


class devx_uar(C.Structure):
    _fields_ = [('reg_addr', C.c_void_p), ('base_addr', C.c_void_p), ('page_id', C.c_uint32),
                ('mmap_off', C.c_long), ('comp_mask', C.c_uint64)]


class _inout(C.Structure):
    _fields_ = [('i', C.c_void_p), ('o', C.c_void_p)]


class dv_obj(C.Structure):                       # qp cq srq rwq dm ah pd devx
    _fields_ = [(n, _inout) for n in ('qp', 'cq', 'srq', 'rwq', 'dm', 'ah', 'pd', 'devx')]


class dv_pd(C.Structure):
    _fields_ = [('pdn', C.c_uint32), ('comp_mask', C.c_uint64)]


class dv_context_attr(C.Structure):
    _fields_ = [('flags', C.c_uint32), ('comp_mask', C.c_uint64), ('fds', C.c_void_p)]


class matcher_attr(C.Structure):
    _fields_ = [('type', C.c_int), ('flags', C.c_uint32), ('priority', C.c_uint16), ('match_criteria_enable', C.c_uint8),
                ('match_mask', C.c_void_p), ('comp_mask', C.c_uint64), ('ft_type', C.c_int), ('ib_port', C.c_uint32)]


class _act_u(C.Union):
    _fields_ = [('ptr', C.c_void_p), ('tag_value', C.c_uint32), ('bulk', C.c_void_p * 2)]


class action_attr(C.Structure):
    _anonymous_ = ('u',)
    _fields_ = [('type', C.c_int), ('u', _act_u)]


def _lib(name):
    for n in (name + '.so.1', C.util.find_library(name.replace('lib', '', 1)), name + '.so'):
        if n:
            try:
                return C.CDLL(n, use_errno=True)
            except OSError:
                pass
    raise OSError('%s not found (rdma-core / OFED user space)' % name)


class Verbs:
    def __init__(self):
        v, m = self.v, self.m = _lib('libibverbs'), _lib('libmlx5')
        P, U32, SZ = C.c_void_p, C.c_uint32, C.c_size_t
        for lib, name, res, args in (
                (v, 'ibv_get_device_list', C.POINTER(P), [C.POINTER(C.c_int)]),
                (v, 'ibv_free_device_list', None, [C.POINTER(P)]),
                (v, 'ibv_get_device_name', C.c_char_p, [P]),
                (v, 'ibv_open_device', P, [P]),
                (v, 'ibv_close_device', C.c_int, [P]),
                (v, 'ibv_alloc_pd', P, [P]),
                (v, 'ibv_reg_mr', C.POINTER(ibv_mr), [P, P, SZ, C.c_int]),
                (v, 'ibv_reg_mr_iova', C.POINTER(ibv_mr), [P, P, SZ, C.c_uint64, C.c_int]),
                (m, 'mlx5dv_open_device', P, [P, C.POINTER(dv_context_attr)]),
                (m, 'mlx5dv_init_obj', C.c_int, [C.POINTER(dv_obj), C.c_uint64]),
                (m, 'mlx5dv_devx_umem_reg', C.POINTER(devx_umem), [P, P, SZ, U32]),
                (m, 'mlx5dv_devx_alloc_uar', C.POINTER(devx_uar), [P, U32]),
                (m, 'mlx5dv_devx_query_eqn', C.c_int, [P, U32, C.POINTER(U32)]),
                (m, 'mlx5dv_devx_obj_create', P, [P, P, SZ, P, SZ]),
                (m, 'mlx5dv_devx_obj_modify', C.c_int, [P, P, SZ, P, SZ]),
                (m, 'mlx5dv_devx_obj_query', C.c_int, [P, P, SZ, P, SZ]),
                (m, 'mlx5dv_devx_obj_destroy', C.c_int, [P]),
                (m, 'mlx5dv_devx_general_cmd', C.c_int, [P, P, SZ, P, SZ]),
                (m, 'mlx5dv_create_flow_matcher', P, [P, C.POINTER(matcher_attr)]),
                (m, '__mlx5dv_create_flow', P, [P, P, SZ, P, SZ])):
            f = getattr(lib, name)
            f.restype, f.argtypes = res, args
            setattr(self, name.lstrip('_'), f)

    @staticmethod
    def errno():
        return C.get_errno()

    # ------------------------------------------------------------------ devices, memory
    def open_dev(self, name, devx):
        n = C.c_int(0)
        lst = self.ibv_get_device_list(C.byref(n))
        ctx = None
        for i in range(n.value):
            if self.ibv_get_device_name(lst[i]).decode() == name:
                if devx:
                    a = dv_context_attr(flags=MLX5DV_CONTEXT_FLAGS_DEVX)
                    ctx = self.mlx5dv_open_device(lst[i], C.byref(a))
                else:
                    ctx = self.ibv_open_device(lst[i])
        self.ibv_free_device_list(lst)
        return ctx

    def pdn(self, pd):
        out = dv_pd()
        o = dv_obj()
        o.pd.i, o.pd.o = pd, C.addressof(out)
        if self.mlx5dv_init_obj(C.byref(o), MLX5DV_OBJ_PD):
            raise OSError(self.errno(), 'mlx5dv_init_obj(PD)')
        return out.pdn

    def reg_mr(self, pd, addr, length, access=IBV_ACCESS_LOCAL_WRITE, iova=None):
        r = self.ibv_reg_mr(pd, addr, length, access) if iova is None else \
            self.ibv_reg_mr_iova(pd, addr, length, iova, access)
        return r.contents if r else None

    def umem_reg(self, ctx, addr, length, access=IBV_ACCESS_LOCAL_WRITE):
        r = self.mlx5dv_devx_umem_reg(ctx, addr, length, access)
        return r.contents if r else None

    def alloc_uar(self, ctx):
        r = self.mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_NC)
        return r.contents if r else None

    def query_eqn(self, ctx, vector=0):
        e = C.c_uint32(0)
        if self.mlx5dv_devx_query_eqn(ctx, vector, C.byref(e)):
            raise OSError(self.errno(), 'mlx5dv_devx_query_eqn')
        return e.value

    # ------------------------------------------------------------------ DevX commands (bytes in, bytearray out)
    def obj_create(self, ctx, inb, outlen):
        i, o = (C.c_char * len(inb)).from_buffer_copy(bytes(inb)), (C.c_char * outlen)()
        h = self.mlx5dv_devx_obj_create(ctx, i, len(inb), o, outlen)
        return h, bytearray(o.raw)

    def obj_modify(self, obj, inb, outlen):
        i, o = (C.c_char * len(inb)).from_buffer_copy(bytes(inb)), (C.c_char * outlen)()
        rc = self.mlx5dv_devx_obj_modify(obj, i, len(inb), o, outlen)
        return rc, bytearray(o.raw)

    def obj_query(self, obj, inb, outlen):
        i, o = (C.c_char * len(inb)).from_buffer_copy(bytes(inb)), (C.c_char * outlen)()
        rc = self.mlx5dv_devx_obj_query(obj, i, len(inb), o, outlen)
        return rc, bytearray(o.raw)

    def general_cmd(self, ctx, inb, outlen):
        i, o = (C.c_char * len(inb)).from_buffer_copy(bytes(inb)), (C.c_char * outlen)()
        rc = self.mlx5dv_devx_general_cmd(ctx, i, len(inb), o, outlen)
        return rc, bytearray(o.raw)

    def obj_destroy(self, obj):
        return self.mlx5dv_devx_obj_destroy(obj)

    # ------------------------------------------------------------------ steering
    def flow(self, ctx, mask, value, actions):
        """A NIC-RX rule: match the outer headers (mask / value = fte_match_param bytes) -> actions [(type, int)]."""
        def params(b):
            p = (C.c_uint8 * (8 + len(b)))()
            C.c_size_t.from_buffer(p).value = len(b)
            C.memmove(C.addressof(p) + 8, bytes(b), len(b))
            return p
        pm, pv = params(mask), params(value)
        ma = matcher_attr(type=IBV_FLOW_ATTR_NORMAL, priority=0, match_criteria_enable=1, match_mask=C.addressof(pm),
                          comp_mask=MLX5DV_FLOW_MATCHER_MASK_FT_TYPE, ft_type=MLX5DV_FLOW_TABLE_TYPE_NIC_RX)
        matcher = self.mlx5dv_create_flow_matcher(ctx, C.byref(ma))
        if not matcher:
            return None, None, 'create_flow_matcher: errno %d' % self.errno()
        acts = (action_attr * len(actions))()
        for k, (t, v) in enumerate(actions):
            acts[k].type = t
            if t == MLX5DV_FLOW_ACTION_TAG:
                acts[k].tag_value = v
            else:
                acts[k].ptr = v
        fl = self.mlx5dv_create_flow(matcher, pv, len(actions), acts, C.sizeof(action_attr))
        if not fl:
            return matcher, None, 'create_flow: errno %d' % self.errno()
        self._keep = getattr(self, '_keep', []) + [pm, pv, acts]
        return matcher, fl, None


def set_data_seg(mem, off, byte_count, lkey, addr):
    """mlx5dv_set_data_seg: a 16-byte receive WQE (one data segment) at mem[off] (a writable numpy uint8 view)."""
    mem[off:off + 16] = np.frombuffer(byte_count.to_bytes(4, 'big') + lkey.to_bytes(4, 'big') + addr.to_bytes(8, 'big'),
                                      np.uint8)


def bar_start(bdf, bar):
    """Bus address of a PCI BAR (sysfs resource file)."""
    with open('/sys/bus/pci/devices/%s/resource' % bdf) as f:
        return int(f.read().split('\n')[bar].split()[0], 16)


def buf_addr(mv):
    """The virtual address of a writable buffer (a pyxrt bo.map() memoryview, an mmap, a numpy array)."""
    return C.addressof(C.c_char.from_buffer(mv))
