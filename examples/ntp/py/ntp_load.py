#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""ntp_load.py -- the no-compile counterpart of ../ntp_load.c: NTP client load generator over UDP, same options,
checks and RESULT line. Slower: one system call per datagram and Python per request.

N worker processes, each with its own connected UDP socket and a window of W requests in flight. Each request is a
48-byte NTPv4 mode-3 packet whose transmit timestamp carries (worker << 48 | seq); a reply is matched by its origin
timestamp, so every reply is checked: mode 4, version 4, origin = a request in flight, receive <= transmit. A request
with no reply after --timeout-us counts as lost. Latency (send -> receive, CLOCK_MONOTONIC) as percentiles.
usage: ntp_load.py --server IP [--port 123] [--threads N] [--window W] [--secs S] [--timeout-us T] [--batch B] [--quiet]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
import udp_load  # noqa: E402

M48 = (1 << 48) - 1


class Ntp:
    mask = M48

    def __init__(self, tid, nthr, a):
        self.tid = tid
        self.head = bytes(((4 << 3) | 3, 0, 6, 0xec)) + bytes(36)

    def preload_items(self):
        return []

    def pick(self):
        return 0, 0

    def build(self, op, key, seq):
        return self.head + (self.tid << 48 | (seq & M48)).to_bytes(8, 'big')

    def parse(self, p, n):
        if n < 48 or (p[0] & 7) != 4 or ((p[0] >> 3) & 7) != 4:
            return None, None                              # counted as bad (ntp_load.c)
        org = int.from_bytes(p[24:32], 'big')
        return 0, (org & M48) if (org >> 48) == self.tid else None

    def account(self, T, op, key, st, p, n):
        if int.from_bytes(p[32:40], 'big') > int.from_bytes(p[40:48], 'big'):
            T.bad += 1


def main():
    ap = argparse.ArgumentParser(description='NTP UDP load generator (Python)')
    ap.add_argument('--server', default='192.168.77.2'); ap.add_argument('--port', type=int, default=123)
    ap.add_argument('--threads', type=int, default=1); ap.add_argument('--window', type=int, default=8)
    ap.add_argument('--secs', type=float, default=5); ap.add_argument('--timeout-us', type=int, default=20000)
    ap.add_argument('--batch', type=int, default=32); ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    a.preload, a.inorder = False, False
    if not (1 <= a.threads <= 64 and 1 <= a.window <= 4096 and 1 <= a.batch <= 64):
        sys.exit('bad args')
    _, T, per, el, _ = udp_load.run(a, Ntp)
    bad = T.bad + T.short
    pv = T.percentiles()
    over = sum(1 for x in T.lat if x // 100 >= udp_load.HB)
    print('RESULT threads=%d window=%d secs=%.1f: %.0f replies/s  sent %d recv %d lost %d (%.4f%%)  bad %d unmatched %d  '
          'lat us p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f (>20ms %d)' %
          (a.threads, a.window, el, T.recv / el, T.sent, T.recv, T.lost, 100.0 * T.lost / T.sent if T.sent else 0.0,
           bad, T.unmatched, pv[0], pv[1], pv[2], pv[3], over))
    if not a.quiet:
        for t, s in enumerate(per):
            print('  thread %d: %.0f replies/s, lost %d, bad %d' % (t, s.recv / el, s.lost, s.bad + s.short))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
