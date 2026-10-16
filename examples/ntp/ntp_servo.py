#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

"""ntp_servo.py -- disciplines the FPGA's time of day (ntp_tod) to this host's clock (CLOCK_REALTIME), through the
host-started ntp_ctl kernel, and publishes the NTP reply fields. Run it in the client network namespace (it measures
the FPGA clock the way any NTP client does: queries to the FPGA server, kernel software timestamps).

  1. rate = 2^64 / (--mhz) per cycle; coarse set from a snapshot (tod read back, host time at the midpoint)
  2. frequency from two NTP-measured offsets --cal-secs apart (snapshot latency is tens of us, NTP ~1 us)
  3. every --interval s: --burst queries, the one with the smallest delay gives the offset theta (FPGA - host);
     PI loop on the rate: base *= 1 - KI * theta / T; applied rate = base * (1 - KP * theta / T), so the phase is
     slewed out over the next interval and the counter never steps backwards (|theta| > --step-ns: one step).
  4. reply fields: host synchronised (chronyc tracking): stratum = host + 1, refid = the host's, root delay / dispersion
     = the host's plus this servo's error; not synchronised: --local-stratum N (refid LOCL), else LI 3, stratum 16.
     LI 3 / stratum 16 until the offset has stayed under --lock-ns for --lock-n intervals in a row.
No PPS: the FPGA clock is exactly as good as the host's, plus the asymmetry of the path measured here (see README).
Writes one JSON line per interval to --status (the latest state) and a log line to stdout."""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ntp_lib import Ctl, Client, ns_to_ntp, ntp_sub_ns  # noqa: E402

KP, KI = 0.7, 0.3

ap = argparse.ArgumentParser()
ap.add_argument("--xclbin", required=True)
ap.add_argument("--bdf", default=None)
ap.add_argument("--server", default="192.168.77.2")
ap.add_argument("--mhz", type=float, default=300.0, help="nominal kernel clock (the servo measures the real one)")
ap.add_argument("--interval", type=float, default=1.0)
ap.add_argument("--burst", type=int, default=8)
ap.add_argument("--cal-secs", type=float, default=2.0)
ap.add_argument("--step-ns", type=float, default=200_000)
ap.add_argument("--lock-ns", type=float, default=5_000)
ap.add_argument("--lock-n", type=int, default=5, help="intervals in a row under --lock-ns before serving as synchronised")
ap.add_argument("--local-stratum", type=int, default=None)
ap.add_argument("--cal-rx-ns", type=float, default=0, help="subtracted from T2 (NIC receive -> fld_rx streaming)")
ap.add_argument("--cal-tx-ns", type=float, default=0, help="added to T3 (T3 -> the frame on the wire)")
ap.add_argument("--secs", type=float, default=0, help="run time (0: until --stop-file)")
ap.add_argument("--stop-file", default=None)
ap.add_argument("--pause-file", default=None, help="no queries (and no corrections) while this file exists")
ap.add_argument("--status", default=None)
a = ap.parse_args()

ctl = Ctl(a.xclbin, a.bdf)
cli = Client(a.server)
T0 = time.time()


def log(*m):
    print("%8.3f" % (time.time() - T0), *m, flush=True)


def best(n):
    rs = [r for r in (cli.query() for _ in range(n)) if r]
    return min(rs, key=lambda r: r["delay"]) if rs else None


def host_status():
    """chronyc -c tracking: (synchronised, stratum, refid int, root delay s, root dispersion s)"""
    try:
        f = subprocess.run(["chronyc", "-c", "tracking"], capture_output=True, text=True, timeout=2).stdout.strip().split(",")
        st = int(f[2]); leap = f[-1]
        return st > 0 and "Not synch" not in leap, st, int(f[0], 16), float(f[10]), float(f[11])
    except Exception:
        return False, 0, 0, 0.0, 0.0


def short(sec):                                     # NTP short format 16.16
    return max(0, min(0xffffffff, int(sec * 65536)))


rate0 = int(2 ** 64 / (a.mhz * 1e6))
ctl.rate(rate0)
ctl.set("cal_rx", int(a.cal_rx_ns * 2 ** 32 / 1e9))
ctl.set("cal_tx", int(a.cal_tx_ns * 2 ** 32 / 1e9))
ctl.set("leap", 3); ctl.set("stratum", 16)


def coarse():
    ta = time.time_ns(); s = ctl.snap(); tb = time.time_ns()
    d = ntp_sub_ns(ns_to_ntp((ta + tb) // 2), s)
    ctl.step_ntp(int(d * 2 ** 32 / 1e9))
    return d, tb - ta


d, w = coarse()
log("COARSE step %+.6f s (snapshot window %.1f us), rate %d (%.3f MHz nominal)" % (d / 1e9, w / 1e3, rate0, a.mhz))
# frequency: snapshot ratio over the calibration time first (good to ~10 ppm), then two NTP offsets
ta = time.time_ns(); s0 = ctl.snap(); tb = time.time_ns()
time.sleep(a.cal_secs)
tc = time.time_ns(); s1 = ctl.snap(); td = time.time_ns()
ratio = ntp_sub_ns(s1, s0) / (((tc + td) - (ta + tb)) / 2)
base = rate0 / ratio
ctl.rate(int(base))
log("FREQ snapshot ratio %.9f -> kernel clock %.6f MHz" % (ratio, a.mhz * ratio))
coarse()
r0 = best(a.burst); time.sleep(a.cal_secs); r1 = best(a.burst)
if r0 and r1:
    fe = (r1["offset"] - r0["offset"]) / ((r1["t1"] - r0["t1"]))
    base *= 1 - fe
    ctl.rate(int(base))
    log("FREQ NTP offsets %.0f -> %.0f ns over %.1f s: %+.3f ppm" % (r0["offset"], r1["offset"], a.cal_secs, fe * 1e6))
r = best(a.burst)
if r and abs(r["offset"]) > a.step_ns:
    ctl.step_ntp(int(-r["offset"] * 2 ** 32 / 1e9))

good = 0
locked = False
nxt = time.time()
while True:
    if a.secs and time.time() - T0 > a.secs:
        break
    if a.stop_file and os.path.exists(a.stop_file):
        break
    nxt += a.interval
    if a.pause_file and os.path.exists(a.pause_file):
        time.sleep(max(0, nxt - time.time())); continue
    r = best(a.burst)
    if not r:
        log("NO-REPLY"); time.sleep(max(0, nxt - time.time())); continue
    th = r["offset"] * 1e-9
    if abs(r["offset"]) > a.step_ns:
        ctl.step_ntp(int(-r["offset"] * 2 ** 32 / 1e9)); good = 0; locked = False
        log("STEP %+.0f ns" % r["offset"])
    else:
        base *= 1 - KI * th / a.interval
        ctl.rate(int(base * (1 - KP * th / a.interval)))
    good = good + 1 if abs(r["offset"]) < a.lock_ns else 0
    synced, hst, hrefid, hrd, hrdisp = host_status()
    now = time.time_ns()
    if good >= a.lock_n:
        locked = True
    if not locked:
        li, stratum, refid, rd, rdisp = 3, 16, 0, 0, 0
    elif synced:
        li, stratum, refid, rd, rdisp = 0, min(15, hst + 1), hrefid, hrd, hrdisp + abs(th) + 1e-6
    elif a.local_stratum is not None:
        li, stratum, refid, rd, rdisp = 0, a.local_stratum, int.from_bytes(b"LOCL", "big"), 0, abs(th) + 1e-6
    else:
        li, stratum, refid, rd, rdisp = 3, 16, 0, 0, 0
    ref = ns_to_ntp(now)
    for k, v in (("refid", refid), ("rdelay", short(rd)), ("rdisp", short(rdisp)), ("ref_lo", ref & 0xffffffff),
                 ("ref_hi", ref >> 32), ("stratum", stratum), ("leap", li)):
        ctl.set(k, v)
    st = dict(t=round(time.time() - T0, 3), offset_ns=round(r["offset"]), delay_ns=r["delay"], res_ns=r["res"],
              ppm=round((rate0 / base - 1) * 1e6, 4), mhz=round(a.mhz * rate0 / base, 6), locked=locked, li=li,
              stratum=stratum, refid=refid.to_bytes(4, "big").hex(), host_synced=synced, sw_t1=r["sw_t1"])
    if a.status:
        with open(a.status + ".tmp", "w") as f:
            json.dump(st, f)
        os.replace(a.status + ".tmp", a.status)
    log("SERVO offset %+7.0f ns  delay %6.0f ns  residence %5.0f ns  clock %.6f MHz  %s stratum %d" %
        (r["offset"], r["delay"], r["res"], st["mhz"], "LOCKED" if locked else "unlocked", stratum))
    time.sleep(max(0, nxt - time.time()))

c = ctl.counters()
log("COUNTERS " + " ".join("%s=%d" % kv for kv in c.items()))
