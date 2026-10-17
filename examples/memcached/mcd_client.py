#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# mcd_client.py -- tests the FPGA memcached (Xilinx HLx pipeline on FLD-lite + udp_lite) over UDP with the memcached
# BINARY protocol, payload = the 24-byte binary header directly (the kernel takes no 8-byte memcached UDP frame header).
#   usage (root, inside the netns that owns the client port): mcd_client.py [fpga ip] [port] [nkeys]
# Checks, each PASS/FAIL: SET -> success; GET -> the value + flags back byte-exact; GET of a never-set key -> miss
# (status 1); overwrite SET -> GET returns the new value; DELETE -> success, then GET -> miss; values 1..2000 B;
# a 2100 B value (DRAM limit 2047 B) must fail cleanly (non-zero status) and the pipeline must keep answering after it;
# then a timed loop of GETs for a rate (Python-bound, not a throughput claim).
import os, random, socket, struct, sys, time

FPGA = (sys.argv[1] if len(sys.argv) > 1 else "192.168.77.2", int(sys.argv[2]) if len(sys.argv) > 2 else 11211)
NKEYS = int(sys.argv[3]) if len(sys.argv) > 3 else 64
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(0.5)
res = []
opaque = 0


def check(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg, flush=True)
    res.append(ok)


def req(op, key=b"", value=b"", extras=b""):
    global opaque
    opaque += 1
    hdr = struct.pack("!BBHBBHIIQ", 0x80, op, len(key), len(extras), 0, 0, len(extras) + len(key) + len(value), opaque, 0)
    s.sendto(hdr + extras + key + value, FPGA)
    try:
        d, a = s.recvfrom(4096)
    except socket.timeout:
        return None
    if len(d) < 24:
        return ("short", d)
    magic, rop, klen, elen, _, status, blen, rop_q, cas = struct.unpack("!BBHBBHIIQ", d[:24])
    body = d[24:24 + blen]
    return {"magic": magic, "op": rop, "status": status, "extras": body[:elen], "key": body[elen:elen + klen],
            "value": body[elen + klen:], "opaque": rop_q, "from": a}


def SET(k, v, flags=0xDEADBEEF):
    return req(0x01, k, v, struct.pack("!II", flags, 0))


def GET(k):
    return req(0x00, k)


def DEL(k):
    return req(0x04, k)


print("FPGA memcached at %s:%d, %d keys" % (FPGA[0], FPGA[1], NKEYS), flush=True)
random.seed(7)
# keys are seeded, not random per run, so every run tests the same keys: a SET whose hash bucket is full is refused
# (4 keys per bucket; with the demo's 128 buckets a random set of 64 keys failed now and then)
keys = [b"key-%04d-%06x" % (i, random.getrandbits(24)) for i in range(NKEYS)]
vals = {k: os.urandom(random.choice([1, 7, 8, 63, 64, 65, 100, 255, 256, 511, 512, 1000, 1400, 2000])) for k in keys}

r = SET(keys[0], vals[keys[0]])
check(r is not None and r.get("magic") == 0x81 and r.get("status") == 0 and r.get("from") == FPGA,
      "first SET answered by the FPGA (magic 0x81, status 0, from %s:%d): %s" % (FPGA[0], FPGA[1], "no reply" if r is None else "status %s" % r.get("status")))
bad = 0
for k in keys[1:]:
    r = SET(k, vals[k])
    if r is None or r.get("status") != 0:
        bad += 1
        print("        SET %s (%d B): %s" % (k.decode(), len(vals[k]), "no reply" if r is None else "status %s" % r.get("status")), flush=True)
check(bad == 0, "SET %d keys (values 1..2000 B): %d failed" % (len(keys) - 1, bad))
bad, flagbad = 0, 0
for k in keys:
    r = GET(k)
    if r is None or r.get("status") != 0 or r["value"] != vals[k]:
        bad += 1
        print("        GET %s: %s" % (k.decode(), "no reply" if r is None else "status %s, %d B" % (r.get("status"), len(r["value"]))), flush=True)
    elif r["extras"] != struct.pack("!I", 0xDEADBEEF):
        flagbad += 1
check(bad == 0 and flagbad == 0, "GET %d keys byte-exact: %d wrong/missing, %d with wrong flags" % (len(keys), bad, flagbad))
r = GET(b"never-set-" + os.urandom(4).hex().encode())
check(r is not None and r.get("status") == 1, "GET of a never-set key -> miss (status 1): %s" % ("no reply" if r is None else r.get("status")))
k = keys[1]; nv = os.urandom(333)
SET(k, nv); r = GET(k)
check(r is not None and r.get("value") == nv, "overwrite SET then GET returns the new value")
r = DEL(keys[2])
check(r is not None and r.get("status") == 0, "DELETE -> success")
r = GET(keys[2])
check(r is not None and r.get("status") == 1, "GET after DELETE -> miss")
r = SET(b"too-big", os.urandom(2100))
check(r is not None and r.get("status") != 0, "SET of a 2100 B value (DRAM limit 2047 B) fails cleanly: %s" % ("no reply" if r is None else "status %s" % r.get("status")))
r = GET(keys[3])
check(r is not None and r.get("value") == vals[keys[3]], "the pipeline still answers correctly after the oversize SET")
n, t0, ok = 2000, time.perf_counter(), 0
for i in range(n):
    r = GET(keys[i % len(keys)])
    ok += r is not None and r.get("status") in (0, 1)
dt = time.perf_counter() - t0
check(ok == n, "%d sequential GETs answered: %d, %.0f GET/s (Python ping-pong, RTT %.1f us)" % (n, ok, n / dt, dt / n * 1e6))
print("MCD-CLIENT %s (%d failed of %d)" % ("PASS" if all(res) else "FAIL", res.count(False), len(res)), flush=True)
