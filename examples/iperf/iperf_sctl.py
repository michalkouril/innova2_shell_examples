#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

"""iperf_sctl.py <xclbin> [--bdf BDF] [--port P] [--mhz F] [--counters] -- the iperf_sctl kernel through pyxrt: set
iperf_srv's UDP port and the kernel clock (cycles per microsecond, for the report's duration and jitter), and print its
counters. Opens the xclbin in a shared context next to e5udp_host. Needs `source /opt/xilinx/xrt/setup.sh`."""
import argparse
import struct
import sys

OP_SET, OP_COUNT = 1, 2
F_PORT, F_CPU = 0, 1
NAMES = ["frames", "data", "fin", "reports", "drop", "tests", "t_rx", "t_bytes_lo", "t_bytes_hi", "t_maxid", "t_lost",
         "t_ooo", "t_dup", "t_dur_lo", "t_dur_hi", "t_jitq4"]


class Sctl:
    def __init__(self, xclbin, bdf=None):
        import pyxrt
        self.x = pyxrt
        self.dev = pyxrt.device(bdf if bdf else 0)
        self.ctx = pyxrt.hw_context(self.dev, self.dev.register_xclbin(pyxrt.xclbin(xclbin)))
        self.k = pyxrt.kernel(self.ctx, "iperf_sctl")
        self.bo = pyxrt.bo(self.dev, 64, pyxrt.bo.normal, self.k.group_id(3))

    def run(self, op, a=0, b=0):
        s32 = lambda v: (v & 0xffffffff) - ((v & 0x80000000) << 1)   # pyxrt rejects ints >= 2^31 for 32-bit args
        self.k(op, s32(a), s32(b), self.bo).wait()

    def set(self, field, value):
        self.run(OP_SET, field, value)

    def counters(self):
        self.run(OP_COUNT)
        self.bo.sync(self.x.xclBOSyncDirection.XCL_BO_SYNC_BO_FROM_DEVICE, 4 * len(NAMES), 0)
        return dict(zip(NAMES, struct.unpack("<%dI" % len(NAMES), bytes(self.bo.read(4 * len(NAMES), 0)))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xclbin"); ap.add_argument("--bdf"); ap.add_argument("--port", type=int); ap.add_argument("--mhz", type=int)
    ap.add_argument("--counters", action="store_true")
    a = ap.parse_args()
    s = Sctl(a.xclbin, a.bdf)
    if a.port is not None:
        s.set(F_PORT, a.port)
    if a.mhz is not None:
        s.set(F_CPU, a.mhz)
    if a.counters:
        c = s.counters()
        mhz = a.mhz or 300
        dur = ((c["t_dur_hi"] << 32) | c["t_dur_lo"]) / (mhz * 1e6)
        byt = (c["t_bytes_hi"] << 32) | c["t_bytes_lo"]
        print("FPGA counters: frames %(frames)d data %(data)d fin %(fin)d reports %(reports)d drop %(drop)d tests %(tests)d" % c)
        print("FPGA last test: %d datagrams received, %d bytes, highest seq %d, lost %d, out of order %d, duplicates %d, "
              "first-to-last %.6f s -> %.0f datagrams/s, %.3f Gb/s payload, jitter %.3f us"
              % (c["t_rx"], byt, c["t_maxid"], c["t_lost"], c["t_ooo"], c["t_dup"], dur,
                 (c["t_rx"] - 1) / dur if dur > 0 else 0, byt * 8 / dur / 1e9 if dur > 0 else 0, c["t_jitq4"] / 16 / mhz))
    return 0


if __name__ == "__main__":
    sys.exit(main())
