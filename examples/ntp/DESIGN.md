<!--
SPDX-FileCopyrightText: 2026 the innova2 contributors
SPDX-License-Identifier: Apache-2.0
-->
# NTP server on the Innova-2 — design

**Status (2026-10-17):** built and tested without PPS — `ntp_tod` / `ntp_resp` / `ntp_ctl` and the host servo
(`ntp_servo.py`) that disciplines the time base to the host's clock; see `README.md`. Not built yet: the PPS input,
the GPS / gpsd servo, CQE hardware timestamps for T2, interleaved mode, rate limiting and NTS (sections below).

A stratum-1 NTP server whose replies are made entirely in the FPGA: requests arrive on a ConnectX-5 port, FLD-lite
hands them to an `ntp_resp` kernel, which stamps and turns them around, and FLD-lite sends the reply. No CPU in the
packet path; the host only disciplines the FPGA clock from a PPS (and labels the seconds).

## Datapath

```
CX5 port -> fld_rx --> ntp_resp (free-running HLS kernel) --> fld_tx -> CX5 port
                          ^    reads TOD at receive (T2) and just before transmit (T3)
                 tod (time-of-day counter) <-- pps_in (FPGA pin, 2-FF sync, edge capture)
                          ^ AXI-lite: rate, phase step, leap/stratum/refid, PPS capture, counters
                    host daemon (servo) <-- GPS NMEA (gpsd) labels each PPS second
```

Same shape as the hls4ml and memcached examples: `e5udp_host` installs the UDP steering rule (UDP port 123) and
runs FLD-lite; `ntp_resp` takes the place of `h4ml_jet`.

## Time base (`tod`)

* 64-bit NTP-format counter (32-bit seconds since 1900, 32-bit fraction) in the kernel clock domain.
* Every cycle: `tod += rate`, where `rate` is the fixed-point fraction per cycle (nominally 2^32 / f_clk, about
  14.3 for 300 MHz, held as 32.32 bits so the frequency can be trimmed in steps far below 1 ppb).
* `phase_step` register: one-shot add (initial set, or a slew spread over N cycles to avoid steps).
* Resolution 3.3 ns at 300 MHz; the counter is the only clock the replies use.

## PPS in

* 3.3 V LVCMOS on an FPGA pin (to be found: the OpenCAPI sideband pair B2/C2 is the candidate; prove the path with
  JTAG boundary scan `SAMPLE` first), through a 2-flip-flop synchroniser and a rising-edge detector.
* On each edge `pps_capture = tod`; `pps_count++`. Sampling uncertainty one clock (3.3 ns) plus synchroniser.
* Input protection (series resistor, ESD) on the cable side; typical PPS is a 100 ms 3.3 V pulse.

## Servo (host, every second)

* Read `pps_capture`; the GPS's NMEA (via gpsd) says which UTC second the edge was; error = capture - that second.
* PI loop: frequency (`rate`) from the integral term, phase via small `phase_step` slews; step once at start-up.
* Publish leap-second warning, stratum (1 locked, 16 unsynchronised), root dispersion from the servo's error.
* Fallback without PPS: discipline `tod` from the ConnectX PHC (ptp4l/phc2sys from a PTP grandmaster) — the host reads
  both clocks back to back; about 1 µs read jitter, averaged.

## Timestamps

* **T2 (receive):** `tod` when `fld_rx` sees the request's completion, minus a calibrated constant (NIC receive path).
  Jitter: FLD-lite's poll interval, sub-µs. Better later: the ConnectX writes a hardware timestamp in every receive
  completion (its free-running clock, ns resolution); map it to `tod` with a linear fit the host maintains.
* **T3 (transmit):** `tod` just before `fld_tx` rings the doorbell, plus a calibrated constant (inline BlueFlame send:
  a fixed ~1-2 µs). Exact alternative: NTP interleaved mode (RFC 5905 / chrony), sending the previous reply's T3 taken
  from the ConnectX's transmit-completion timestamp.
* Calibrate both constants once with a hardware-timestamping client on the looped-back second port.

## `ntp_resp` kernel

* Parse Ethernet / IPv4 (no options) / UDP dport 123; NTP header 48 bytes; accept mode 3 (client), version 3 or 4;
  drop anything else (count it).
* Reply in place: swap MACs, IPs, ports; LI from the leap register; VN copied; mode 4; stratum; poll copied;
  precision -28 (about 3.7 ns); root delay 0; root dispersion from the host; refid "GPS" / "PPS";
  reference timestamp = last PPS; origin = the request's transmit timestamp; receive = T2; transmit = T3.
* IPv4 and UDP checksums: offloaded to the ConnectX on send (FLD-lite already sets L3|L4 checksum flags).
* Rate limit / kiss-o'-death per source: optional, a small hash table. NTS (authenticated NTP, AES-SIV-CMAC): later.

## Expected performance

* Rate: FLD-lite's small-frame rate, about 2.4-2.6 M replies/s per port (90-byte frames), constant latency ~2 µs.
* Server-side timestamp error: tens of ns with CQE timestamps; clients see their network path asymmetry on top.

## Test

* chrony (or `ntpdate -q`) in a network namespace on the second ConnectX port, as the hls4ml client; check offset,
  stratum, refid, and that every reply's origin matches the request.
* Load: an `mcd_load`-style multi-threaded UDP generator for replies/s and loss.
* Accuracy: a client using ConnectX hardware timestamps on the looped port, compared against the PPS.

## Effort

Kernel and its testbench 1-2 days; host servo and gpsd hook 1 day; calibration and tests 1 day — once a PPS pin is
proven.
