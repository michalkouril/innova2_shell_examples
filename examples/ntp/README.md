# ntp — an NTP server answered by the FPGA

NTP client requests arrive on ConnectX port 0, FLD-lite hands them to `ntp_resp`, which timestamps them, turns them
into replies in place and hands them back to FLD-lite to send: no CPU in the packet path. Design notes: `DESIGN.md`.

```
fld_rx -> ntp_resp -> fld_tx            ntp_resp <-> ntp_tod (time of day)          ntp_ctl <- host (ntp_servo.py)
```

| kernel | what it does |
|---|---|
| `ntp_tod` | free-running, II=1: a 96-bit time-of-day accumulator (units of 2^-64 s; its top 64 bits are the NTP timestamp, 32-bit seconds since 1900 + 32-bit fraction). Every cycle `tod += rate`; the host sets the rate (frequency, steps of ~2e-11) and adds phase steps. Serves timestamp requests without ever pausing the count. |
| `ntp_resp` | free-running: per frame, T2 = tod right after the first beat arrives; accepts Ethernet / IPv4 without options / UDP port 123 / a 48-byte NTP mode-3 request, version 3 or 4; replies in place (MACs, IPs, ports swapped; the NIC recomputes the checksums) with mode 4, the version echoed, leap / stratum / precision / root delay / root dispersion / refid / reference timestamp as the host set them, origin = the request's transmit timestamp, receive = T2 − cal_rx, transmit = T3 + cal_tx with T3 taken just before the beat that carries it. Anything else is dropped and counted (length, header, mode / version). |
| `ntp_ctl` | host-started, one command per run: rate, phase step, snapshot of `ntp_tod`; set a reply field of `ntp_resp`; read both kernels' counters. |

`ntp_servo.py` (pyxrt, in the client network namespace) disciplines `ntp_tod` to the host's clock: coarse set from a
snapshot, the kernel clock's frequency from snapshots and then from NTP-measured offsets, then once per second a burst
of queries (kernel software timestamps on the client socket, the smallest-delay sample) and a PI loop on the rate that
slews the phase out over the next second (the count never goes backwards; offsets above 200 µs are stepped). Stratum is
the host's + 1 with the host's refid when chrony on the host is synchronised; otherwise `--local-stratum N` (refid
`LOCL`), or leap 3 / stratum 16. Until the offset has stayed under 5 µs for five intervals in a row it serves leap 3 / stratum 16.

**No PPS in this version.** `DESIGN.md`'s PPS input (an FPGA pin, edge capture of `tod`) is not built: no pin of the
card has been proven to carry one. The FPGA clock is as good as the host's clock, plus the asymmetry of the path the
servo measures through (NIC receive → `fld_rx` streaming before T2, T3 → BlueFlame send after it; the `cal_rx` /
`cal_tx` fields correct for it, both 0 here, uncalibrated).

## Run

```
bash examples/ntp/build.sh                       # or build_all.sh ntp
bash examples/ntp/run.sh [xclbin]                # on the card's host (see the top-level README: DAC loop, P2P)
```

`run.sh`: `e5udp_host --port 123` on port 0; `ntp_servo.py` until it locks (5 intervals in a row under 5 µs), then 10 s more; `ntp_client.py` (verdict `NTP-CLIENT
PASS`): 2,000 v4 + 200 v3 queries, every reply checked (mode 4, version echoed, origin = our transmit timestamp,
stratum / refid / leap as the servo published, receive ≤ transmit), offset / delay / residence percentiles, then six
frames the server must ignore (mode 1, mode 4, version 2, version 5, 40- and 120-byte payloads), each of which must
raise exactly its drop counter; `chronyd -Q` (chrony as a stock client, 8 samples); `ntp_load` (C, threads × window
in flight, sendmmsg / recvmmsg, every reply matched by its origin timestamp) for replies/s, loss and latency.
`NTP_LOCAL_STRATUM` (default 10) is served when the host's own clock is not synchronised.

The C testbench: `g++ -O1 -I<Vitis_HLS>/include -o ntp_tb kernels/ntp_kernel_tb.cpp && ./ntp_tb` (`NTP-TB PASS`:
v4 / v3 replies byte-exact against a reference, 10 kinds of frame dropped into the right counters, `ntp_tod` = a
96-bit model through rate change, ±steps and a snapshot).

## Results

Run of 2026-10-17 on hpy440 (Xeon E5-1603 v4, inn2x r10, XRT 2.27), `xclbin/ntp.xclbin` (data clock 300 MHz, link
WNS +0.004 ns). The host's own clock was not synchronised (no NTP source on that network), so the server ran with
`--local-stratum 10`, refid `LOCL`.

| | |
|---|---|
| servo | coarse set, then the kernel clock measured: 300.00898 MHz (+29.9 ppm against nominal; snapshot estimate +49 ppm, NTP estimate −19 ppm on top); locked 13 s after start; over 32 locked intervals (including the load runs) offset mean −5 ns, rms 264 ns, max 755 ns |
| `ntp_client.py` | **NTP-CLIENT PASS**: 2,200 replies (2,000 v4 + 200 v3), all mode 4 with the version echoed, origin = our transmit timestamp, stratum 10 / LOCL / leap 0; offset FPGA − host p5 −786 ns, p50 −155 ns, p95 +102 ns (one outlier −39 µs: a client-side scheduling delay); round trip p50 8.3 µs (min 7.2) through the host's UDP stack; FPGA residence T3 − T2 170 ns (169–171) |
| drops | mode 1, mode 4, version 2, version 5, 40-byte and 120-byte payloads: no reply, and each raised exactly its counter (drop_mode / drop_len) |
| `chronyd -Q` | "System clock wrong by -0.000002 seconds": chrony, as a stock client, agrees with the host clock to 2 µs |
| load (`ntp_load`, replies/s, 0 lost in every run) | 1 × 1: 90,189 (p50 11.1 µs); 1 × 32: 423,711; 2 × 32: 747,226; **4 × 32: 1,382,076** (p50 73 µs, p99 361 µs) — 13.2 M requests in the run, every reply matched by its origin timestamp, 0 bad |
| accounting | fld_rx 13,218,849 frames, fld_tx 13,218,843 replies: the 6 the drop tests sent; `DMA failed` 0 |

The load runs are limited by the client (4 cores of Linux UDP sockets in one namespace), not by the server: `ntp_resp` spends 170 ns per
frame, and FLD-lite's small-frame rate is about 2.4–2.6 M frames/s (`DESIGN.md`'s estimate; not measured with NTP). `e5udp_host`
prints `E5UDP-HOST PROBLEM` because it expects every frame echoed; `run.sh` checks the difference against
`ntp_resp`'s drop counters instead (`ACCOUNTED`).

What is not measured here: accuracy against a real reference. Both ends of every measurement are this host's clock,
so the numbers show how closely the FPGA follows it, through a path whose asymmetry (NIC → `fld_rx` before T2,
T3 → wire after it) is not calibrated (`cal_rx` = `cal_tx` = 0). A PPS input or ConnectX hardware timestamps on a
second host are the next steps (`DESIGN.md`).
