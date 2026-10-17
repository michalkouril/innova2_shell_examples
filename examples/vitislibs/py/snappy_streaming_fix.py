#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-FileCopyrightText: Advanced Micro Devices, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# snappy_streaming_fix.py <xclbin> [options as snappy_streaming.py] -- pyxrt host for the overlay benchmark
# data_compression/L2/demos/snappy_streaming_fix (AMD's snappy_streaming with our fix of the chunk-header loop,
# amd_ip_bugs V18): the same flow as snappy_streaming.py, at full size -- 32 MB of the host's copyright texts in 64 KB
# blocks, FPGA round trip of all of it, and FPGA decompression of a 4 MB stream encoded in Python (README: 222 MB/s
# compress, 1.79 GB/s decompress).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snappy_streaming import main

if __name__ == '__main__':
    sys.exit(main('snappy_streaming_fix', 32, 64, 0, 4))
