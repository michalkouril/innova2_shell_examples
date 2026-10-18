#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""mcd_load.py -- the no-compile counterpart of ../mcd_load.c: memcached BINARY-protocol load generator over UDP, same
options, phases, checks and RESULT line. Slower: one system call per datagram and Python per request.

Payload = the 24-byte binary header + extras + key + value (the FPGA memcached takes no memcached UDP frame header;
--frame adds the standard 8-byte one, for stock memcached -U). N worker processes, each with its own connected UDP
socket and a window of W requests in flight. The opaque field carries (worker << 24 | seq & 0xFFFFFF); --inorder
matches each response to the worker's oldest outstanding request instead (the FPGA memcached does not return the
opaque field but answers one flow in order). Phases: preload (SET every key once, the workers splitting the key
range), then a timed run of GETs or a GET/SET mix (--set-pct). Values are a deterministic function of (key, size),
so every --check-every'th GET hit is compared byte for byte.
usage: mcd_load.py --server IP [--port 11211] [--threads N] [--window W] [--keys K] [--vsize V] [--secs S]
                   [--set-pct P] [--timeout-us T] [--check-every C] [--frame] [--no-preload] [--batch B] [--quiet]
                   [--inorder]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'common'))
import udp_load  # noqa: E402

M32 = 0xFFFFFFFF


def value_of(key, n):
    """The deterministic value of (key, size), as mcd_load.c value_of()."""
    x = (key * 2654435761 ^ n * 40503) & M32
    v = bytearray(n)
    for i in range(n):
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        v[i] = x & 0xFF
    return bytes(v)


class Mcd:
    mask = 0xFFFFFF

    def __init__(self, tid, nthr, a):
        self.tid, self.a = tid, a
        self.rng = (0x9E3779B9 * (tid + 1)) & M32
        self.k0, self.k1 = a.keys * tid // nthr, a.keys * (tid + 1) // nthr
        self.vals = {}

    def value(self, key):
        v = self.vals.get(key)
        if v is None:
            v = self.vals[key] = value_of(key, self.a.vsize)
        return v

    def preload_items(self):
        return [(1, k) for k in range(self.k0, self.k1)]

    def pick(self):
        r = self.rng
        r ^= (r << 13) & M32
        r ^= r >> 17
        r ^= (r << 5) & M32
        self.rng = r
        return (1 if ((r >> 8) % 100) < self.a.set_pct else 0), r % self.a.keys

    def build(self, op, key, seq):
        opq = (self.tid << 24 | (seq & 0xFFFFFF)) & M32
        k = b'k%08u' % key
        ext = b'\xde\xad\xbe\xef\x00\x00\x00\x00' if op == 1 else b''
        val = self.value(key) if op == 1 else b''
        h = bytes((0x80, op, 0, len(k), len(ext), 0, 0, 0)) + (len(ext) + len(k) + len(val)).to_bytes(4, 'big') + \
            opq.to_bytes(4, 'big') + bytes(8)
        f = ((opq & 0xFFFF).to_bytes(2, 'big') + b'\x00\x00\x00\x01\x00\x00') if self.a.frame else b''
        return f + h + ext + k + val

    def parse(self, p, n):
        o = 8 if self.a.frame else 0
        if n - o < 24 or p[o] != 0x81:
            return None, None
        opq = int.from_bytes(p[o + 12:o + 16], 'big')
        st = p[o + 6] << 8 | p[o + 7]
        return st, (opq & 0xFFFFFF) if (opq >> 24) == self.tid else None

    def account(self, T, op, key, st, p, n):
        if op == 1:
            if st == 0:
                T.set_ok += 1
            else:
                T.err += 1
        elif st == 1:
            T.miss += 1
        elif st != 0:
            T.err += 1
        else:
            T.hit += 1
            if self.a.check_every and T.hit % self.a.check_every == 0:
                o = 8 if self.a.frame else 0
                el, kl = p[o + 4], p[o + 2] << 8 | p[o + 3]
                bl = int.from_bytes(p[o + 8:o + 12], 'big')
                vl = bl - el - kl
                T.checked += 1
                v0 = o + 24 + el + kl
                if vl != self.a.vsize or o + 24 + bl > n or bytes(p[v0:v0 + vl]) != self.value(key):
                    T.bad += 1


def main():
    ap = argparse.ArgumentParser(description='memcached binary-protocol UDP load generator (Python)')
    ap.add_argument('--server', default='192.168.77.2'); ap.add_argument('--port', type=int, default=11211)
    ap.add_argument('--threads', type=int, default=1); ap.add_argument('--window', type=int, default=8)
    ap.add_argument('--keys', type=int, default=64); ap.add_argument('--vsize', type=int, default=32)
    ap.add_argument('--secs', type=float, default=10); ap.add_argument('--set-pct', type=int, default=0)
    ap.add_argument('--timeout-us', type=int, default=20000); ap.add_argument('--check-every', type=int, default=64)
    ap.add_argument('--batch', type=int, default=32); ap.add_argument('--frame', action='store_true')
    ap.add_argument('--no-preload', dest='preload', action='store_false'); ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--inorder', action='store_true')
    a = ap.parse_args()
    if not (1 <= a.threads <= 64 and 1 <= a.window <= 4096 and 0 <= a.vsize <= 9000 - 128 and 1 <= a.batch <= 64):
        sys.exit('bad args')
    pre, T, per, el, tpre = udp_load.run(a, Mcd)
    if a.preload:
        print('preload: %d keys x %d B in %.2f s: SET ok %d, failed %d, lost %d' % (a.keys, a.vsize, tpre, pre.set_ok, pre.err, pre.lost))
    pv = T.percentiles()
    over = sum(1 for x in T.lat if x // 100 >= udp_load.HB)
    rx_wire = (T.rx_bytes + T.recv * 66.0) * 8 / el / 1e9
    tx_wire = (T.tx_bytes + T.sent * 66.0) * 8 / el / 1e9
    print('RESULT threads=%d window=%d keys=%d vsize=%d set_pct=%d secs=%.1f: %.0f req/s  sent %d recv %d lost %d (%.3f%%)  '
          'hit %d miss %d err %d set_ok %d/%d  checked %d bad %d unmatched %d short %d  '
          'lat us p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f (>20ms %d)  resp %.3f Gb/s wire (req %.3f)' %
          (a.threads, a.window, a.keys, a.vsize, a.set_pct, el, T.recv / el, T.sent, T.recv, T.lost,
           100.0 * T.lost / T.sent if T.sent else 0.0, T.hit, T.miss, T.err, T.set_ok, T.sets, T.checked, T.bad,
           T.unmatched, T.short, pv[0], pv[1], pv[2], pv[3], over, rx_wire, tx_wire))
    if not a.quiet:
        for t, s in enumerate(per):
            print('  thread %d: %.0f req/s, lost %d, err %d, bad %d' % (t, s.recv / el, s.lost, s.err, s.bad))
    return 1 if (T.bad or T.err) else 0


if __name__ == '__main__':
    sys.exit(main())
