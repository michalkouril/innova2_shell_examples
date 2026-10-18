# iperf — an iperf2 UDP server in the FPGA

A stock `iperf -c <ip> -u` (iperf2) on the host talks to a server that runs entirely in the FPGA, inline in FLD-lite:
the ConnectX-5 steers UDP port 5001 to `fld_rx`, `iperf_srv` counts every datagram, and when the client's final
datagram arrives, `iperf_srv` answers with iperf2's own server report, so the client prints the FPGA's numbers as its
"Server Report" line. No CPU touches the datagrams.

```
CX5 port 0 -> fld_rx --> iperf_srv --> fld_tx -> CX5 port 0     (only the reports go back out)
                          ^ trq/ttk: iperf_tmr (cycle counter)
                          ^ scfg/srq, scnt: iperf_sctl (host-started: port, kernel clock, counters)
```

## Kernels (`kernels/iperf_kernel.cpp`)

* **`iperf_srv`** (free-running) checks Ethernet / IPv4 (no options) / UDP to the configured port and reads iperf2's
  datagram header (iperf 2.2.1 `include/payloads.h`, `struct UDP_datagram`: `id`, `tv_sec`, `tv_usec`, `id2`,
  big-endian; the sequence number is the signed 64-bit `id2:id`, negative on the final datagrams). It keeps iperf2's
  server accounting (`src/Reporter.c`): a sequence number below the next expected one counts as out of order, a jump
  counts the skipped ones as lost, the datagram total is the highest sequence number seen, bytes are UDP payload bytes,
  and jitter is RFC 1889 on arrival time minus the client's send time.
* **The report** is iperf2's (`src/Reports.c`, `write_UDP_AckFIN`): a zeroed `UDP_datagram` followed by the 112-byte
  `struct server_hdr` (flags `HEADER_VERSION1 | HEADER_SEQNO64B`; bytes, duration = first to last arrival, lost
  (iperf2's error count, lost minus out of order), out of order, datagrams, jitter; the extension words are zero). It
  goes back in place: addresses and ports swapped, lengths set, checksums computed by the NIC.
* **Tests in a row** need no reload: the state stays until the first data datagram after a final one starts a new test,
  so the client's retried finals (every 250 ms until a report arrives) all get the same report.
* **`iperf_tmr`** is the clock (a free-running 64-bit cycle counter; a timer in `iperf_srv`'s own loop would stop with
  back-pressure). **`iperf_sctl`** (host-started, `iperf_sctl.py` over pyxrt) sets the port and the kernel clock in
  cycles per microsecond, and reads the counters: cumulative frames / data / finals / reports / drops / tests, and the
  current test's received datagrams, bytes, highest sequence number, lost, out of order, duplicates, first-to-last
  time and jitter.

`kernels/iperf_kernel_tb.cpp` (C simulation) replays two tests of iperf2 datagrams -- loss, swapped pairs, a duplicate,
varying arrival spacing, a retried final, then a second test with no reset -- and checks each report frame byte for
byte against a reference computed from the datagrams sent:

```
g++ -O1 -std=c++14 -I<Vitis_HLS>/include -o iperf_tb kernels/iperf_kernel_tb.cpp kernels/iperf_kernel.cpp && ./iperf_tb
```

## Build and run

```
bash examples/iperf/build.sh           # on the build machine (see the top-level README)
bash examples/iperf/run.sh             # on the card's host: IPERF_LENS="64 1470 8192", IPERF_SECS=5
```

`run.sh` brings up the endpoint (jumbo frames), sets the port and the clock, and for each datagram size runs
`iperf -c 192.168.77.2 -u -l <len> -b 10G` from the network namespace on port 1, then prints the client's line, the
FPGA's Server Report, the FPGA's counters and the NIC's receive counter on port 0. One iperf2 client socket sends at
most about 0.6-0.8 M datagrams/s, so at small sizes the client, not the FPGA, sets the rate. Three short tests back to
back follow, then the check that `fld_tx` sent exactly the reports.

Limits: one test at a time (iperf2's `-P` parallel streams would be counted as one); IPv4 without options; the
duration and jitter use the kernel clock the host sets (`--mhz`, read from the xclbin by `run.sh`).

## Results

HP Z440 (Xeon E5-1603 v4, 4 cores), inn2x r10, XRT 2.27, iperf 2.2.1; the ConnectX-5 ports run at **25 GbE** (25G
DAC); `xclbin/iperf.xclbin` (data clock 300 MHz); `run.sh`, 2026-10-17. The FPGA's counts equal the NIC's receive
counter on port 0 in every test (plus the final datagrams), 0 receive drops, `fld_tx` sent exactly the 14 reports.

| test | the client's Server Report (the FPGA's numbers) | FPGA counters |
|---|---|---|
| `-l 64 -b 10G`, 1 stream | 249 Mb/s, 0/2,434,880 lost, 486,973 pps | 53 out of order (one client socket's rate) |
| `-l 1470 -b 10G`, 1 stream | 5.23 Gb/s, 0/2,222,695 lost, 444,525 pps, jitter < 1 us | client-bound |
| `-l 8192 -b 10G`, 1 stream (jumbo) | 10.7 Gb/s, 0/818,686 lost, jitter 0.002 ms | client-bound |
| `-l 64`, 4 streams (`-P 4`) | (merged: one test at a time) | **1.86 M datagrams/s** received |
| `-l 1470`, 4 streams | | **1.70 M datagrams/s, 20.0 Gb/s** |
| `-l 8192`, 4 streams | | **378 k datagrams/s, 24.8 Gb/s payload = 25 GbE line rate** |
| 3 x `-l 1470 -b 1G -t 1` back to back | 1.07 Gb/s each, 0 lost, separate reports | no reload in between |

The host's 4 cores limit the client below line rate for small datagrams; the FPGA kept up with everything sent.

## The iperf2 UDP client in the FPGA (`build_client.sh`, `run_client.sh`)

The other direction: ETH Zurich's fpga-network-stack `iperf_udp` client (BSD-3-Clause; `fetch_fns.sh` clones it at the
validated commit, it is not part of this repository) sends from the FPGA to a stock `iperf -s -u` on the host.
`kernels/iperf_fns.cpp` wraps it unchanged on FLD-lite + `udp_rx_lite` / `udp_tx_lite` (fns's controls come from
`iperf_ctl`, its experiment timer `clock()` runs as its own kernel `iperf_clock`, on the platform's free-running clock);
`kernels/iperf_fns_tb.cpp` checks the datagram stream in C simulation. `e5udp_host --iperf IP,WORDS,GAP,DELAY` starts
an experiment; `run_client.sh` runs one per datagram size from a fresh load.

Results (same host and cards, `xclbin/iperf_client.xclbin`, `run_client.sh`, 2026-10-17): the FPGA sends at **25 GbE
line rate** in both runs -- 1472 B datagrams 2.03 M/s (23.9 Gb/s of payload), 8192 B 378 k/s (24.8 Gb/s) -- for fns's
experiment of 781.25 M cycles of clock id 4 (50 MHz here: 15.6 s); the NIC's receive counter on port 1 equals
`fld_tx`'s frame count in both. The stock `iperf -s -u` on the host receives only 6.9 Gb/s (1472 B) and 17.8 Gb/s
(8192 B) of it, so its per-second lines show 71 % / 28 % "lost": the losses are the host server's, not the link's.

