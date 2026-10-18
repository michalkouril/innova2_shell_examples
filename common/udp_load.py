# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""udp_load -- the engine of the Python UDP load generators (examples/memcached/py/mcd_load.py,
examples/ntp/py/ntp_load.py), the no-compile counterparts of mcd_load.c / ntp_load.c.

Same scheme as the C programs: N workers, each with its own connected UDP socket (own source port) and a window of W
requests in flight; a request is matched to its slot when the reply comes back (by an id the protocol echoes, or --
inorder -- to the worker's oldest outstanding request), its latency is taken from the slot's send time, and a request
with no reply after timeout_us counts as lost and frees its slot. Workers are processes, not threads (the GIL would
serialise threads), started together through a barrier. Python has no sendmmsg / recvmmsg, so each datagram is one
system call: expect several times fewer requests/s per worker than the C programs.

A protocol object provides:  build(op, key, seq) -> bytes;  pick(seq) -> (op, key) for the timed run;
parse(buf, n) -> (status or None if malformed, the echoed id or None);  account(stats, op, key, status, buf, n);
preload_items() -> [(op, key)];  mask: the bits of the request sequence number the echoed id carries.
"""
import multiprocessing as mp
import socket
import time

import numpy as np

HB = 200000                     # latency histogram bins of 100 ns -> 20 ms


class Stats:
    FIELDS = ('sent', 'recv', 'lost', 'err', 'miss', 'hit', 'sets', 'set_ok', 'checked', 'bad', 'unmatched', 'short',
              'tx_bytes', 'rx_bytes', 'over')

    def __init__(self):
        for f in self.FIELDS:
            setattr(self, f, 0)
        self.lat = []

    def merge(self, o):
        for f in self.FIELDS:
            setattr(self, f, getattr(self, f) + getattr(o, f))
        self.lat.extend(o.lat)

    def percentiles(self, pct=(50, 90, 99, 99.9)):
        if not self.lat:
            return [-1.0] * len(pct)
        a = np.asarray(self.lat, dtype=np.int64) // 100
        h = np.bincount(a[a < HB], minlength=HB)
        cum = np.cumsum(h)
        out = []
        for p in pct:
            want = int(self.recv * p / 100.0)
            i = int(np.searchsorted(cum, want)) if want else -1
            out.append(i / 10.0 if want and i < HB else -1.0)
        return out


def run_phase(sock, proto, T, win, batch, timeout_us, inorder, items=None, t_end=None):
    """items: a list of (op, key) to send once each (preload); else the timed run until t_end (time.monotonic_ns)."""
    busy = [False] * win
    sseq = [0] * win
    st = [0] * win
    sop = [0] * win
    skey = [0] * win
    seq = head = inflight = nxt = 0
    lim = timeout_us * 1000
    total = len(items) if items is not None else 0
    rx = bytearray(65536)
    while True:
        t = time.monotonic_ns()
        more = nxt < total if items is not None else t < t_end
        if not more and inflight == 0:
            break
        nb = 0
        while more and inflight < win and nb < batch:
            i = seq % win
            if busy[i]:
                break
            if items is not None:
                op, key = items[nxt]
                nxt += 1
                more = nxt < total
            else:
                op, key = proto.pick()
            pkt = proto.build(op, key, seq)
            while True:
                try:
                    sock.send(pkt)
                    break
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError as e:          # ENOBUFS: the socket buffer is full, retry
                    if e.errno != 105:
                        raise
            busy[i], sseq[i], st[i], sop[i], skey[i] = True, seq, t, op, key
            seq += 1
            inflight += 1
            nb += 1
            T.tx_bytes += len(pkt)
            T.sets += op == 1
        T.sent += nb
        got = 0
        while got < 64:
            try:
                n = sock.recv_into(rx)
            except (BlockingIOError, InterruptedError):
                break
            got += 1
            tr = time.monotonic_ns()
            T.rx_bytes += n
            status, mseq = proto.parse(rx, n)
            if status is None:
                T.short += 1
                continue
            if inorder:
                while head != seq and not busy[head % win]:
                    head += 1
                if head == seq:
                    T.unmatched += 1
                    continue
                i = head % win
                head += 1
            else:
                if mseq is None:
                    T.unmatched += 1
                    continue
                i = mseq % win
                if not busy[i] or (sseq[i] & proto.mask) != mseq:
                    T.unmatched += 1
                    continue
            lat = tr - st[i]
            T.lat.append(lat)
            T.recv += 1
            proto.account(T, sop[i], skey[i], status, rx, n)
            busy[i] = False
            inflight -= 1
        if got == 0 or inflight == win:
            tr = time.monotonic_ns()
            for i in range(win):
                if busy[i] and tr - st[i] > lim:
                    busy[i] = False
                    inflight -= 1
                    T.lost += 1


def _worker(tid, nthr, args, make_proto, bar, q):
    proto = make_proto(tid, nthr, args)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    s.connect((args.server, args.port))
    s.setblocking(False)
    T = Stats()
    bar.wait()                                             # 1: start
    items = proto.preload_items() if getattr(args, 'preload', False) else None
    if items:
        run_phase(s, proto, T, args.window, args.batch, args.timeout_us, args.inorder, items=items)
    q.put(('pre', tid, T))
    bar.wait()                                             # 2: preload done everywhere
    t_end = bar_t_end(args)
    bar.wait()                                             # 3: go
    T = Stats()
    run_phase(s, proto, T, args.window, args.batch, args.timeout_us, args.inorder, t_end=t_end.value)
    q.put(('run', tid, T))


def bar_t_end(args):
    return args._t_end


def run(args, make_proto):
    """Start args.threads workers; returns (preload Stats, run Stats, per-worker run Stats, elapsed s, preload s)."""
    n = args.threads
    ctx = mp.get_context('fork')
    bar = ctx.Barrier(n + 1)
    q = ctx.Queue()
    args._t_end = ctx.Value('q', 0)
    ps = [ctx.Process(target=_worker, args=(t, n, args, make_proto, bar, q)) for t in range(n)]
    for p in ps:
        p.start()
    tp0 = time.monotonic_ns()
    bar.wait()
    pre = Stats()
    for _ in range(n):
        pre.merge(q.get()[2])
    bar.wait()
    tpre = (time.monotonic_ns() - tp0) / 1e9
    args._t_end.value = time.monotonic_ns() + int(args.secs * 1e9)
    t0 = time.monotonic_ns()
    bar.wait()
    tot, per = Stats(), [None] * n
    for _ in range(n):
        _, tid, T = q.get()
        per[tid] = T
        tot.merge(T)
    for p in ps:
        p.join()
    return pre, tot, per, (time.monotonic_ns() - t0) / 1e9, tpre
