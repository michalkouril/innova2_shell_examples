# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""pyxrt_util -- the small layer the Python (pyxrt) hosts of examples/suite/py and examples/vitislibs/py share.

    from pyxrt_util import Card
    c = Card(xclbin)                      # device: $INNOVA2_DEV (BDF or index), default 0
    k = c.kernel("krnl_vadd")             # first CU of that kernel; c.kernel("k:{k_1}") picks one
    a = c.bo(k, 0, nbytes)                # buffer in the memory bank of k's argument 0
    a.put(np_array); r = c.call(k, a, b, n); a.get(np.uint32)
    t = c.timed(k, a, b, n)               # one run, wall time in seconds (start .. wait, host-measured)

Needs XRT's Python binding: `source /opt/xilinx/xrt/setup.sh` (it puts pyxrt on PYTHONPATH), and numpy.
Traps handled here: pyxrt rejects a 32-bit kernel argument >= 2^31 given as a Python int, so `arg32()` passes it as
signed; buffers are allocated in the bank of the kernel argument (group_id), never a fixed bank.
"""
import os
import time

import numpy as np
import pyxrt

TO_DEV = pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_TO_DEVICE
FROM_DEV = pyxrt.xclBOSyncDirection.XCL_BO_SYNC_BO_FROM_DEVICE


def arg32(v):
    """A 32-bit unsigned value as the signed int pyxrt accepts for an int/uint argument."""
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= (1 << 31) else v


class Buf:
    """A device buffer and its host mapping (numpy views of it)."""

    def __init__(self, dev, size, group):
        self.size = size
        self.bo = pyxrt.bo(dev, max(size, 4), pyxrt.bo.normal, group)
        self.mem = np.frombuffer(self.bo.map(), dtype=np.uint8)

    def put(self, data, offset=0):
        """Copy bytes / a numpy array into the buffer and sync it to the device."""
        b = np.frombuffer(memoryview(data).cast('B'), dtype=np.uint8) if not isinstance(data, np.ndarray) \
            else data.view(np.uint8).ravel()
        self.mem[offset:offset + b.size] = b
        self.bo.sync(TO_DEV, b.size, offset)
        return self

    def zero(self):
        self.mem[:] = 0
        self.bo.sync(TO_DEV, self.size, 0)
        return self

    def get(self, dtype=np.uint8, count=None, offset=0):
        """Sync from the device and return a copy as a numpy array of dtype."""
        n = self.size - offset if count is None else count * np.dtype(dtype).itemsize
        self.bo.sync(FROM_DEV, n, offset)
        return self.mem[offset:offset + n].view(dtype).copy()

    def address(self):
        return self.bo.address()


class Card:
    def __init__(self, xclbin, dev=None):
        d = os.environ.get('INNOVA2_DEV', '0') if dev is None else dev
        self.dev = pyxrt.device(int(d) if str(d).isdigit() else d)
        self.xclbin_path = xclbin
        self.xb = pyxrt.xclbin(xclbin)
        self.ctx = pyxrt.hw_context(self.dev, self.dev.register_xclbin(self.xb))
        self.kernels = {}

    def kernel(self, name):
        if name not in self.kernels:
            self.kernels[name] = pyxrt.kernel(self.ctx, name)
        return self.kernels[name]

    def bo(self, k, arg, size):
        """A buffer of size bytes in the bank kernel k's argument arg is connected to."""
        return Buf(self.dev, size, k.group_id(arg))

    @staticmethod
    def _args(args):
        return [a.bo if isinstance(a, Buf) else a for a in args]

    def start(self, k, *args):
        """Start one run (returns the pyxrt.run; call .wait())."""
        return k(*self._args(args))

    def call(self, k, *args):
        r = k(*self._args(args))
        st = r.wait()
        if st != pyxrt.ert_cmd_state.ERT_CMD_STATE_COMPLETED:
            raise RuntimeError('kernel run ended in state %s' % st)
        return st

    def timed(self, k, *args):
        """One run; returns the host-measured time from start to completion, in seconds (includes the ~10-30 us of
        XRT dispatch, so short kernels read longer than the device-side time AMD's OpenCL hosts report)."""
        a = self._args(args)
        t0 = time.perf_counter()
        r = k(*a)
        st = r.wait()
        t = time.perf_counter() - t0
        if st != pyxrt.ert_cmd_state.ERT_CMD_STATE_COMPLETED:
            raise RuntimeError('kernel run ended in state %s' % st)
        return t


def result(name, ok, detail=''):
    """The PASS / FAIL line every Python host ends with (run_py.sh greps for it)."""
    print('%s %s%s' % ('PYHOST-PASS' if ok else 'PYHOST-FAIL', name, (': ' + detail) if detail else ''), flush=True)
    return 0 if ok else 1


def start_args(k, args):
    """Start a run of kernel k with only the arguments in args ({index: Buf | int}); for kernels with AXI-stream
    ports, whose stream arguments cannot be passed positionally. Returns the started pyxrt.run."""
    r = pyxrt.run(k)
    for i, v in args.items():
        r.set_arg(i, v.bo if isinstance(v, Buf) else v)
    r.start()
    return r


def wait_ok(r, timeout_s=None):
    """Wait for a run (at most timeout_s seconds when given); True if it completed. A run still going after the
    timeout means a stuck CU: loading another xclbin resets it."""
    st = r.wait() if timeout_s is None else r.wait(int(timeout_s * 1000))
    return st == pyxrt.ert_cmd_state.ERT_CMD_STATE_COMPLETED


def timed_timeout(c, k, *args, timeout_s=120):
    """As Card.timed, but gives up after timeout_s seconds (RuntimeError) instead of waiting forever on a stuck CU."""
    a = c._args(args)
    t0 = time.perf_counter()
    r = k(*a)
    if not wait_ok(r, timeout_s):
        raise RuntimeError('kernel run did not complete within %d s (stuck CU: load another xclbin to reset it)' % timeout_s)
    return time.perf_counter() - t0
