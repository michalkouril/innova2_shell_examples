# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""ifc -- the ConnectX PRM command layouts, read at run time from the host's own OFED mlx5_ifc.h.

The C hosts include mlx5_ifc.h (through ../devx/prm.h) and let the compiler turn each `u8 field[width]` bit-array into
an offset. This module does the same in Python, so no offset is copied by hand and none can go stale against the
headers the host's OFED ships:

    L = Ifc()                                  # finds /usr/src/ofa_kernel/<arch>/<kernel>/include/linux/mlx5/mlx5_ifc.h
    buf = L.buf('create_cq_in')                # zeroed bytearray of MLX5_ST_SZ_BYTES(create_cq_in)
    L.set(buf, 'create_cq_in', 'cq_context.log_cq_size', 11)     # MLX5_SET; nested fields by dotted path
    L.set64(buf, 'create_cq_in', 'cq_context.dbr_addr', off)     # MLX5_SET64
    L.get(out, 'create_cq_out', 'cqn')         # MLX5_GET
    L.enum('MLX5_CMD_OP_CREATE_CQ')            # an enum / #define constant

MLX5_IFC_H (the file) or MLX5_IFC_DIR (OFED's include/linux/mlx5) override the search. Layout rules, as the kernel's
device.h macros: struct mlx5_ifc_X_bits is a sequence of bit fields in wire order (big-endian); a field of <= 32 bits
lies inside one big-endian dword, a 64-bit field is one big-endian qword; a union is as wide as its widest member.
"""
import glob
import os
import platform
import re

_TOK = re.compile(r'0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|[A-Za-z_]\w*|<<|>>|\S')


def find_header():
    if os.environ.get('MLX5_IFC_H'):
        return os.environ['MLX5_IFC_H']
    if os.environ.get('MLX5_IFC_DIR'):
        return os.path.join(os.environ['MLX5_IFC_DIR'], 'mlx5_ifc.h')
    c = sorted(glob.glob('/usr/src/ofa_kernel/*/%s/include/linux/mlx5/mlx5_ifc.h' % platform.release())) or \
        sorted(glob.glob('/usr/src/ofa_kernel/*/*/include/linux/mlx5/mlx5_ifc.h')) or \
        sorted(glob.glob('/usr/src/ofa_kernel/default/include/linux/mlx5/mlx5_ifc.h'))
    if not c:
        raise FileNotFoundError('mlx5_ifc.h not found (the OFED / DOCA-Host source tree under /usr/src/ofa_kernel; '
                                'or set MLX5_IFC_DIR to its include/linux/mlx5)')
    return c[0]


class Ifc:
    def __init__(self, path=None):
        self.path = path or find_header()
        src = open(self.path).read()
        fpga = os.path.join(os.path.dirname(self.path), 'mlx5_ifc_fpga.h')   # the FPGA capability layouts it refers to
        if os.path.exists(fpga):
            src = open(fpga).read() + '\n' + src
        src = src.replace('\\\n', ' ')
        src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
        src = re.sub(r'//[^\n]*', ' ', src)
        self.consts = {}
        body = []
        for l in src.split('\n'):
            m = re.match(r'\s*#\s*define\s+(\w+)\s+(.+)$', l)
            if m:
                try:
                    self.consts[m.group(1)] = self._eval(m.group(2))
                except Exception:
                    pass
            if not l.lstrip().startswith('#'):
                body.append(l)
        self.t = _TOK.findall('\n'.join(body))
        self.types = {}            # 'struct mlx5_ifc_x_bits' -> ('struct'|'union', members)
        self._size = {}
        self._parse()

    # ------------------------------------------------------------------ constants
    def _eval(self, e):
        e = re.sub(r'\bBIT(_ULL)?\s*\(', '(1<<', e)
        e = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
        e = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(self.consts.get(m.group(0), m.group(0))), e)
        if re.search(r'\b[A-Za-z_]', re.sub(r'0[xX][0-9a-fA-F]+', '0', e)):
            raise ValueError(e)
        return int(eval(e, {'__builtins__': {}}))

    def enum(self, name):
        return self.consts[name]

    # ------------------------------------------------------------------ parser
    def _parse(self):
        t, i = self.t, 0
        while i < len(t):
            if t[i] == 'enum':
                j = i + 1 if t[i + 1] == '{' else i + 2
                if t[j] == '{':
                    i = self._enum(j + 1)
                    continue
            if t[i] in ('struct', 'union') and re.match(r'\w+$', t[i + 1]) and t[i + 2] == '{':
                mem, j = self._body(i + 3)
                self.types['%s %s' % (t[i], t[i + 1])] = (t[i], mem)
                i = j
                continue
            i += 1

    def _enum(self, i):
        t, val = self.t, -1
        while t[i] != '}':
            name = t[i]
            i += 1
            if t[i] == '=':
                j, depth = i + 1, 0
                while depth or t[j] not in (',', '}'):
                    depth += (t[j] == '(') - (t[j] == ')')
                    j += 1
                try:
                    val = self._eval(' '.join(t[i + 1:j]))
                except Exception:
                    val = None
                i = j
            elif val is not None:
                val += 1
            if val is not None:
                self.consts[name] = val
            if t[i] == ',':
                i += 1
        return i + 1

    def _dims(self, i):
        dims = []
        while self.t[i] == '[':
            j = self.t.index(']', i)
            dims.append(self._eval(' '.join(self.t[i + 1:j])) if j > i + 1 else 0)
            i = j + 1
        return dims, i

    def _body(self, i):
        """Members up to the matching '}' -> ([(name, kind, type, dims)], index after '}'). kind: 'bits' (type None),
        'type' (a named struct / union), 'anon' (an inline struct / union: type = (kind, members); name None when
        its members are reached directly, as C11 anonymous members)."""
        t, mem = self.t, []
        while t[i] != '}':
            if t[i] == 'u8':
                name = t[i + 1]
                dims, i = self._dims(i + 2)
                mem.append((name, 'bits', None, dims))
                i += 1
            elif t[i] in ('struct', 'union') and t[i + 1] == '{':
                kind = t[i]
                sub, i = self._body(i + 2)
                name = None
                if t[i] != ';':
                    name = t[i]
                    i += 1
                mem.append((name, 'anon', (kind, sub), []))
                i += 1
            elif t[i] in ('struct', 'union'):
                dims, j = self._dims(i + 3)
                mem.append((t[i + 2], 'type', '%s %s' % (t[i], t[i + 1]), dims))
                i = j + 1
            elif t[i] == 'DECLARE_FLEX_ARRAY':          # DECLARE_FLEX_ARRAY(struct x_bits, name): no space
                j = t.index(')', i)
                mem.append((t[j - 1], 'type', '%s %s' % (t[i + 2], t[i + 3]), [0]))
                i = j + 2
            else:
                i += 1
        return mem, i + 1

    # ------------------------------------------------------------------ layout
    def _tdef(self, typ):
        if isinstance(typ, tuple):
            return typ
        for k in (typ, 'struct mlx5_ifc_%s_bits' % typ, 'union mlx5_ifc_%s_bits' % typ):
            if k in self.types:
                return self.types[k]
        raise KeyError('no struct/union %s in %s' % (typ, self.path))

    def _msize(self, m):
        name, kind, typ, dims = m
        n = 1
        for d in dims:
            n *= d
        return n if kind == 'bits' else n * self.bits(typ)

    def bits(self, typ):
        """Size of a struct / union in bits."""
        key = typ if isinstance(typ, str) else id(typ)
        if key not in self._size:
            kind, mem = self._tdef(typ)
            sz = [self._msize(m) for m in mem]
            self._size[key] = (max(sz) if sz else 0) if kind == 'union' else sum(sz)
        return self._size[key]

    def size(self, typ):
        """MLX5_ST_SZ_BYTES"""
        return self.bits(typ) // 8

    def buf(self, typ):
        return bytearray(self.size(typ))

    def _find(self, typ, name):
        """(bit offset, member) of a member of typ, looking through anonymous members."""
        kind, mem = self._tdef(typ)
        off = 0
        for m in mem:
            if m[0] == name:
                return off, m
            if m[0] is None and m[1] == 'anon':
                r = self._find(m[2], name)
                if r:
                    return off + r[0], r[1]
            if kind == 'struct':
                off += self._msize(m)
        return None

    def field(self, typ, path):
        """(bit offset, bit width) of a dotted field path inside typ."""
        off, t = 0, typ
        parts = path.split('.')
        for k, p in enumerate(parts):
            r = self._find(t, p)
            if r is None:
                raise KeyError('%s has no field %s (in %s)' % (t if isinstance(t, str) else 'anonymous', p, path))
            off += r[0]
            m = r[1]
            if k < len(parts) - 1:
                t = m[2]
        return off, self._msize(m)

    def off(self, typ, path):
        """MLX5_ADDR_OF as a byte offset"""
        return self.field(typ, path)[0] // 8

    def set(self, b, typ, path, v, base=0):
        o, w = self.field(typ, path)
        if w == 64:
            return self.set64(b, typ, path, v, base)
        if w > 32 or (o % 32) + w > 32:
            raise ValueError('%s.%s: %d bits at bit %d is not inside one dword' % (typ, path, w, o))
        dw = base + (o // 32) * 4
        sh = 32 - w - (o % 32)
        mask = ((1 << w) - 1) << sh
        cur = int.from_bytes(b[dw:dw + 4], 'big')
        b[dw:dw + 4] = ((cur & ~mask) | ((int(v) << sh) & mask)).to_bytes(4, 'big')

    def set64(self, b, typ, path, v, base=0):
        o, w = self.field(typ, path)
        if w != 64 or o % 64:
            raise ValueError('%s.%s is not an aligned 64-bit field' % (typ, path))
        b[base + o // 8:base + o // 8 + 8] = int(v).to_bytes(8, 'big')

    def get(self, b, typ, path, base=0):
        o, w = self.field(typ, path)
        if w == 64:
            return int.from_bytes(b[base + o // 8:base + o // 8 + 8], 'big')
        dw = base + (o // 32) * 4
        return (int.from_bytes(b[dw:dw + 4], 'big') >> (32 - w - (o % 32))) & ((1 << w) - 1)
