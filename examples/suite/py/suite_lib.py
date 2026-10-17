# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""suite_lib -- what the Python ports of the suite's hosts share: finding an example's xclbin(s), its test data."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', '..', 'common')))
from pyxrt_util import Card, Buf, result, arg32  # noqa: E402,F401

BUILD_SUB = 'build_dir.hw.innova2_innova2_xdma_202320_4'


def xclbin(path, name):
    """path: the xclbin itself, the example's build_dir, or the example directory; name: the xclbin's file name."""
    if path.endswith('.xclbin'):
        return path
    for d in (path, os.path.join(path, BUILD_SUB)):
        if os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name)
    sys.exit('*** %s not found under %s' % (name, path))


def parse(desc, extra=None):
    """The ports' command line: <xclbin | build_dir | example dir> [options]."""
    p = argparse.ArgumentParser(description=desc)
    p.add_argument('path', help='the xclbin, or the build_dir / example directory holding it')
    if extra:
        extra(p)
    return p.parse_args()


def rand31(n, seed=1):
    """n random ints in [0, 2^31) -- the range of the hosts' std::rand(); numpy's generator, not glibc's sequence."""
    return np.random.default_rng(seed).integers(0, 2 ** 31, n, dtype=np.int64).astype(np.int32)


def i32(x):
    """Wrap an integer array to int32, as C int arithmetic on the card does."""
    return np.asarray(x).astype(np.int64).astype(np.int32)
