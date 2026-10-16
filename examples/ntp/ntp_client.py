#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

"""ntp_client.py -- checks the FPGA NTP server (run in the client network namespace while ntp_servo.py runs).
  1. --n v4 queries and --n/10 v3 queries: every reply mode 4, version echoed, origin = our transmit timestamp,
     stratum / refid / leap as the servo published (--status), receive <= transmit, T1 <= T4; offset (FPGA - host),
     delay and the FPGA residence time (T3 - T2) as percentiles
  2. frames the server must not answer (mode 1, mode 4, version 2, version 5, 40-byte and 120-byte payloads): no reply,
     and the matching ntp_resp drop counter (read through ntp_ctl) grows by exactly one each
Verdict: NTP-CLIENT PASS / FAIL."""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ntp_lib import Ctl, Client  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--xclbin", required=True)
ap.add_argument("--bdf", default=None)
ap.add_argument("--server", default="192.168.77.2")
ap.add_argument("--n", type=int, default=2000)
ap.add_argument("--status", default=None, help="ntp_servo.py --status file: the fields the replies must carry")
ap.add_argument("--pause-file", default=None, help="ntp_servo.py --pause-file: created during the drop tests")
a = ap.parse_args()

fails = []
cli = Client(a.server)
st = json.load(open(a.status)) if a.status else None


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p / 100))]


rs = []
for i in range(a.n + a.n // 10):
    vn = 4 if i < a.n else 3
    r = cli.query(vn=vn)
    if not r:
        fails.append("query %d (v%d): no reply" % (i, vn)); continue
    if r["mode"] != 4 or r["vn"] != vn:
        fails.append("query %d: mode %d version %d" % (i, r["mode"], r["vn"]))
    if r["rec"] > r["xmt"] or r["t4"] < r["t1"]:
        fails.append("query %d: receive > transmit or T4 < T1" % i)
    if st and (r["stratum"] != st["stratum"] or r["refid"].hex() != st["refid"] or r["li"] != st["li"]):
        fails.append("query %d: stratum %d refid %s li %d, servo published %d %s %d" %
                     (i, r["stratum"], r["refid"].hex(), r["li"], st["stratum"], st["refid"], st["li"]))
    rs.append(r)
n = len(rs)
off = [r["offset"] for r in rs]; dl = [r["delay"] for r in rs]; res = [r["res"] for r in rs]
print("CLIENT %d replies (%d v4 + %d v3), origin echoed in all; stratum %s refid %s li %s; T1 from the TX error queue in %d"
      % (n, a.n, a.n // 10, rs[-1]["stratum"] if rs else "-", rs[-1]["refid"] if rs else "-", rs[-1]["li"] if rs else "-",
         sum(r["sw_t1"] for r in rs)))
if rs:
    print("  offset FPGA - host ns: min %.0f p5 %.0f p50 %.0f p95 %.0f max %.0f" % (min(off), pct(off, 5), pct(off, 50), pct(off, 95), max(off)))
    print("  delay ns:              min %.0f p50 %.0f p99 %.0f" % (min(dl), pct(dl, 50), pct(dl, 99)))
    print("  FPGA residence T3-T2 ns: min %.0f p50 %.0f max %.0f" % (min(res), pct(res, 50), max(res)))

# frames that must be dropped
ctl = Ctl(a.xclbin, a.bdf)


def raw(lvm, length):
    p = struct.pack("!BBbb II 4s QQQQ", lvm, 0, 6, -20, 0, 0, b"\0" * 4, 0, 0, 0, 0x0123456789abcdef)
    return (p + b"\0" * 80)[:length]


cases = [("mode 1", raw(4 << 3 | 1, 48), "drop_mode"), ("mode 4", raw(4 << 3 | 4, 48), "drop_mode"),
         ("version 2", raw(2 << 3 | 3, 48), "drop_mode"), ("version 5", raw(5 << 3 | 3, 48), "drop_mode"),
         ("40 B payload", raw(4 << 3 | 3, 40), "drop_len"), ("120 B payload (extension field)", raw(4 << 3 | 3, 120), "drop_len")]
if a.pause_file:
    open(a.pause_file, "w").close(); time.sleep(2.5)                # the servo's current burst is over
for what, pkt, ctr in cases:
    c0 = ctl.counters()
    cli.send_raw(pkt)
    rep = cli.recv_raw()
    time.sleep(0.01)
    c1 = ctl.counters()
    grew = {k: c1[k] - c0[k] for k in ("frames", "replies", "drop_len", "drop_hdr", "drop_mode")}
    ok = rep is None and grew[ctr] == 1 and grew["frames"] == 1 and grew["replies"] == 0
    print("  %-34s %s: reply %s, counters %s" % (what, "dropped" if ok else "WRONG", "none" if rep is None else len(rep), grew))
    if not ok:
        fails.append(what)
if a.pause_file:
    os.remove(a.pause_file)
c = ctl.counters()
print("COUNTERS " + " ".join("%s=%d" % kv for kv in c.items()))
for f in fails[:20]:
    print("  FAIL", f)
print("NTP-CLIENT %s (%d problems)" % ("PASS" if not fails and n == a.n + a.n // 10 else "FAIL", len(fails)))
