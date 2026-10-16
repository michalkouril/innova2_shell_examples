#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""h4ml_client.py <fpga ip> <port> <jets.bin> [max] -- drive the h4ml_jet endpoint (FLD-lite + hls4ml jet tagger).
jets.bin: records of 16 x int16 features (raw ap_fixed<16,6>), 5 x int16 expected scores (hls4ml's bit-accurate
emulation) and the true label (uint8). Sends every jet as a 52-byte "JET1" datagram with a bounded window, matches the
replies by sequence number and checks scores, class and status bit for bit; then a ping-pong latency run.
Prints H4ML-CLIENT PASS/FAIL. Standard library only."""
import socket, struct, sys, time, select
ip, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
mx = int(sys.argv[4]) if len(sys.argv) > 4 else None
REC = struct.Struct('<16h5hB'); raw = open(path, 'rb').read()
recs = [REC.unpack_from(raw, i) for i in range(0, len(raw), REC.size)][:mx]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
s.connect((ip, port))
def pkt(seq, x): return b'JET1' + struct.pack('<I', seq) + struct.pack('<16h', *x) + bytes(12)
def chk(seq, r):
    if len(r) != 52 or r[:4] != b'JET1': return 'bad reply'
    sq = struct.unpack_from('<I', r, 4)[0]
    if sq != seq: return 'seq'
    rec = recs[seq]; x, y, lab = rec[:16], rec[16:21], rec[21]
    if struct.unpack_from('<16h', r, 8) != tuple(x): return 'features changed'
    if struct.unpack_from('<5h', r, 40) != tuple(y): return 'scores differ'
    if r[50] != max(range(5), key=lambda c: (y[c], -c)) or r[51] != 1: return 'class/status'
    return None
# 1. every jet, window W
W = 32; nxt = 0; got = {}; bad = 0; first_bad = None; t0 = time.time(); last_rx = t0
while len(got) < len(recs):
    while nxt < len(recs) and nxt - len(got) < W:
        s.send(pkt(nxt, recs[nxt][:16])); nxt += 1
    if not select.select([s], [], [], 0.5)[0]:
        if time.time() - last_rx > 3: break
        continue
    r = s.recv(2048); last_rx = time.time(); sq = struct.unpack_from('<I', r, 4)[0] if len(r) >= 8 else -1
    if 0 <= sq < len(recs) and sq not in got:
        e = chk(sq, r); got[sq] = r[50]; 
        if e: bad += 1; first_bad = first_bad or (sq, e)
dt = time.time() - t0
lost = len(recs) - len(got)
acc = sum(1 for q, c in got.items() if c == recs[q][21]) / max(1, len(got))
print('jets %d  replies %d  lost %d  mismatching %d%s  %.0f jets/s (Python client, window %d)  accuracy vs labels %.4f'
      % (len(recs), len(got), lost, bad, '' if not first_bad else ' first %s' % (first_bad,), len(got) / dt, W, acc))
# 2. ping-pong latency
rt = []
for i in range(2000):
    q = i % len(recs); t = time.perf_counter(); s.send(pkt(q, recs[q][:16]))
    if not select.select([s], [], [], 1)[0]: continue
    r = s.recv(2048); rt.append(time.perf_counter() - t)
    if chk(q, r): bad += 1
rt.sort()
print('ping-pong %d/2000 answered, RTT median %.1f us, p99 %.1f us' % (len(rt), 1e6 * rt[len(rt) // 2], 1e6 * rt[int(len(rt) * .99)]))
ok = lost == 0 and bad == 0 and len(rt) == 2000
print('H4ML-CLIENT %s' % ('PASS' if ok else 'FAIL')); sys.exit(0 if ok else 1)
