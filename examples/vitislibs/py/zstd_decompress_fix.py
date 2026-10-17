#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# zstd_decompress_fix.py <xclbin> [options as zstd_decompress.py] -- pyxrt host for the overlay benchmark
# data_compression/L2/demos/zstd_decompress_fix (AMD's zstd_decompress with our three fixes, amd_ip_bugs V17): the
# same flow as zstd_decompress.py, level 19 by default (README: 966 / 1149 / 912 MB/s at levels 1 / 9 / 19 on 32 MB of
# text; --levels 1,9,19 --reset-xclbin <other xclbin> runs all three, the kernel decodes one stream per load).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zstd_decompress import main

if __name__ == '__main__':
    sys.exit(main('zstd_decompress_fix', '19'))
