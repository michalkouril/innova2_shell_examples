# innova2_shell_examples — accelerator examples for the Innova-2 (inn2x r10)

Examples for an Innova-2 Flex card (Xilinx XCKU15P + ConnectX-5) running the **inn2x r10 ("ert4") shell**, platform
`innova2_innova2_xdma_202320_4`. Most of them use what makes this card different from an Alveo board: the FPGA drives
the ConnectX-5's queues itself (FLD-lite), so packets go NIC → FPGA → NIC with no CPU in the data path.

Every xclbin in `xclbin/` was built from this tree with `build_all.sh` against the published dev platform and tested on
a card; the table gives the result of that run (see "Results").

| example | what it shows | kernels |
|---|---|---|
| `examples/fld_echo` | FLD-lite line-rate echo: the FPGA polls the ConnectX's completion queues and rings its doorbells; an on-FPGA generator / checker drives it through a DAC loop | fld_rx, afu_loop, fld_tx, fld_reap, e5_ticker2, gen_tx, sink_rx |
| `examples/zuc` | ZUC (3GPP 128-EEA3 / 128-EIA3) encryption / integrity inline in the network path, 4 cores | FLD-lite + zuc_in, zuc_k (RTL), zuc_out + generator / checker |
| `examples/memcached` | a memcached server (binary protocol over UDP) in the FPGA; hash table and values in DDR | FLD-lite + udp_rx_lite, mcd, mcd_mem, mcd_dbg, udp_tx_lite |
| `examples/ntp` | an NTP server answered by the FPGA: a disciplined time-of-day counter, replies stamped and built in place (no PPS yet: the host's clock is the reference) | FLD-lite + ntp_resp, ntp_tod, ntp_ctl |
| `examples/iperf` | an iperf2 UDP server in the FPGA: a stock `iperf -c -u` gets its "Server Report" from the card (counted, lost, out of order, jitter, pps); also the FPGA as the iperf2 client (ETH Zurich fpga-network-stack, fetched at build time) | FLD-lite + iperf_srv, iperf_tmr, iperf_sctl; client: udp_rx_lite, iperf_udp_fns, iperf_clock, iperf_ctl, udp_tx_lite |
| `examples/hls4ml` | neural-network inference per packet: hls4ml's jet tagger (16 → 64 → 32 → 32 → 5) answers UDP requests | FLD-lite + h4ml_jet |
| `examples/compression` | LZ4 and GZip from AMD Vitis Libraries: LZ4 with 2 / 4 / 8 engines, GZip 2 / 8 cores, GZip decompression, compress + decompress in one xclbin | xilLz4Compress, xilGzipCompBlock, xilDecompress |
| `examples/lat_probe` | kernel-to-memory latency per memory type (on-chip window, DDR) | lat_probe |
| `examples/hostmem` | a kernel reading and writing host RAM through the shell's peer window | hostmem_probe |
| `examples/rsa` | RSA-2048 signing: CRT halves as 1024-bit modular exponentiations on 9 engines, every signature verified on the host (pyxrt, no host build) | rsa_modexp |
| `tools/figz` | **`figz`**, used like `gzip`, compressing and decompressing on the card (Python, no build step) | the `gzip_figz` xclbin (or `gzip_combo`) |
| `examples/suite` | 20 examples from AMD Vitis_Accel_Examples, re-pinned to this card's memory; AMD's C++ hosts, or Python (pyxrt) ports of them in `py/` | (upstream) |
| `examples/vitislibs` | 18 AMD Vitis Libraries benchmarks (3 with our fixes to AMD bugs: jpegDec, zstd, snappy; AES as the 4-engine kernel of AMD's published figure) built with their own Makefiles (C++ hosts, or the Python (pyxrt) hosts in `py/`) and compared with AMD's published numbers (crc32, adler32, AES, resize, jpegDec, LZ4/zstd/snappy, compound sort, 5 graph algorithms on SuiteSparse graphs, SVD, quadrature, solvers); results and the ones that do not fit in `examples/vitislibs/README.md`; no xclbins in `xclbin/` | (upstream) |

`fld_lite/` holds what the network examples share: the FLD-lite kernels (`e5/fld_lite.cpp`), the generator / checker
(`e5/gen`), UDP (`e5/udp_lite.cpp`), the ZUC AFU (`e5/zuc`), the host programs (`e5/*_host.cpp`) with their DevX
queue helpers (`e1`–`e3`, `s0`), the `innova2_peermem` module and the earlier E1–E5 experiments (see `fld_lite/README.md`).

## What you need

On the card's host:
* the card flashed with inn2x r10 and its host files installed (`innova2_shell`, package `innova2-xdma` 202320.4.0-1),
  XRT 2.27 with that release's xocl patches (P2P buffers on kernel ≥ 6.17), and `innova2_py_app` ≥ 1.3.23
  (`innova2_ddr_mode`);
* for the network examples: OFED with DevX (DOCA-Host 3.5 `doca-roce` or MLNX_OFED), kernel headers, P2P enabled
  (`sudo xrt-smi configure --p2p enable`, then a warm reboot), and **ConnectX port 1 cabled to port 0** (a DAC loop):
  the client or the generator on one port talks to the FPGA on the other;
* for `compression`: `lz4`, `gzip`, `ocl-icd-opencl-dev`, `libboost-dev`, and the HLS arbitrary-precision headers
  (`ap_int.h` …, from a Vitis HLS install: `HLS_INCLUDE=<Vitis_HLS>/include`).

To build xclbins: Vitis 2023.2, XRT, and the dev platform from `innova2_shell_dev` (package `innova2-xdma-dev`, or its
`hw.xsa` parts reassembled). Plan on 30–90 minutes and 8–12 GB of RAM per link.

## Run the prebuilt xclbins

```
git clone https://github.com/michalkouril/innova2_shell_examples.git && cd innova2_shell_examples
sha256sum -c xclbin/SHA256SUMS
bash fld_lite/host_setup.sh                 # host programs + innova2_peermem.ko
bash examples/hls4ml/run.sh                 # likewise memcached, ntp, iperf, zuc, fld_echo, lat_probe, hostmem
HLS_INCLUDE=... bash examples/compression/host/build_hosts.sh && bash examples/compression/run.sh
```

The run scripts find the card and its ConnectX-5 by themselves (`common/card_env.sh`; with several cards set
`USER_BDF`), need passwordless `sudo`, switch the card to HYBRID DDR mode for the network examples and back to `on`
when they exit, and print the number of `DMA failed` kernel messages during the run. Each takes an optional xclbin path.
The suite: build it (below), copy `$BUILD/suite` to the host, rebuild its host programs there against the
host's XRT (`bash examples/suite/build_hosts.sh <that dir>`), then `bash examples/suite/run.sh <that dir>`.

## Build from source

```
source <Vitis 2023.2>/settings64.sh; source /opt/xilinx/xrt/setup.sh
export PLATFORM=<path>/innova2_innova2_xdma_202320_4.xpfm   # or install innova2-xdma-dev
export BUILD=<scratch dir>                                    # default ./build
./build_all.sh -j 2                    # every example; or name some: ./build_all.sh zuc compression/lz4x8
bash examples/suite/build.sh           # the 20-example suite (SUITE_JOBS=2)
```

Results go to `$BUILD/xclbin/<example>.xclbin`. Each link passes `common/link_gate.sh` (routing legal, no unscalable clock
missing timing; it prints the clocks v++ chose — several examples run their data clock below 300 MHz, which v++
selects by itself). Examples that write to the ConnectX through the PEER window get `innova2_peer_fixup.sh` from the
dev platform's `tools/`. `compression` fetches AMD Vitis Libraries v2023.2_update1 and `suite` AMD Vitis_Accel_Examples
at a pinned commit.

## Results

On an HP Z440 (Xeon E5-1603 v4, 4 cores; card 0000:04:00, inn2x r10, XRT 2.27, Ubuntu 26.04), with the xclbins in
`xclbin/` (`SHA256SUMS`), from a clean copy of this tree: `fld_lite/host_setup.sh`, then each run script. The network
examples, lat_probe, hostmem and the Python hosts were run on 2026-10-17; rsa, compression, figz and the suite's C++
hosts on 2026-10-16 (their kernels have not changed since).
Data clock = the frequency v++ chose for the kernels (the platform asks for 300 MHz). No run logged a `DMA failed`.

| example | data clock | result |
|---|---|---|
| fld_echo | 300 MHz | 200,000 frames at gap 0 each size, 0 TX / RX errors, 0 mismatches: 2.75 M frames/s at 64 B, 1.64 M at 256 B, 1.57 M at 512 B, lossless at 1024 B (1.21 M frames/s, 9.94 Gb/s of frames) and 1500 B (0.79 M, 9.42 Gb/s) -- FLD-lite's own rates: the ports run at 25 GbE; 64 B idle echo round trip 4.29 µs mean (p99 5.45), of which the FPGA's RX CQE → TX CQE 1.87 µs |
| zuc | 227 MHz | 0 mismatches in every run. Integrity 8191 B 19.45 Gb/s, 4096 B 16.05 Gb/s; confidentiality 8191 B 9.67 Gb/s, 4096 B 9.13 Gb/s; paced 4096 B 7.24 Gb/s lossless |
| memcached | 300 MHz | client 10/10 PASS (SET / GET byte-exact / miss / overwrite / DELETE / oversize SET refused); 65,536 keys preloaded (18 refused: full buckets, as C simulation predicts); 0 lost in every run: 86.8k req/s at p50 11.3 µs (window 1); GETs 1.12 M req/s (4 threads × 16) and 1.44 M req/s (4 × 64); GET/SET mixes 1.13 M (10 % SETs), 1.06 M (50 %), 1.28 M req/s (100 %, 4 × 64) -- the client host's 4 cores set these rates. Hash table 131,072 buckets × 4 keys, value store 524,288 × 2 KB; `kernels/mcd_load_tb.cpp` checks the stall and overwrite fixes and the capacity in C simulation |
| ntp | 300 MHz | NTP-CLIENT PASS: 2,200 replies, every origin echoed, v3 and v4; 6 invalid requests dropped and counted; FPGA − host offset p50 +231 ns (p5 −466, p95 +783); servo locked, last offsets +190 / +214 / −112 ns; `chronyd -Q` 3 µs; FPGA residence 170 ns; load 1.39 M replies/s (4 threads × 32), 0 lost of 13.1 M |
| iperf | 300 MHz | stock iperf 2.2.1 `iperf -c -u` against the FPGA, every test 0 lost, its Server Report from the card: 1 stream 249 Mb/s at 64 B (487k pps), 5.23 Gb/s at 1470 B, 10.7 Gb/s at 8 KB; 4 streams (FPGA / NIC counters): 1.86 M datagrams/s at 64 B, 20.0 Gb/s at 1470 B, **24.8 Gb/s at 8 KB = 25 GbE line rate**; tests back to back without a reload. The ports run at 25 GbE (25G DAC); the FPGA as the client (fns iperf_udp, `iperf_client.xclbin`) sends at 25 GbE line rate: 2.03 M datagrams/s at 1472 B, 24.8 Gb/s at 8 KB (NIC counters = fld_tx frames) |
| hls4ml | 241 MHz | 166,000 jets, 0 lost, every score equal to hls4ml's bit-accurate model, 60.4k jets/s (Python client); accuracy vs labels 0.763; ping-pong RTT median 23.0 µs |
| lat_probe | 300 MHz | per access, DDR mode `on`: write+ack 88 cycles, read 104 cycles; HYBRID URAM window: 70 / 72 cycles |
| hostmem | 300 MHz | kernel ↔ host RAM: 32768 beats each way, 0 wrong; 6.97 GB/s write, 7.43 GB/s read; dependent read 1.05 µs |
| rsa | 213 MHz | 1,100 RSA-2048 signatures × 10 rounds, all verified on the host (0 failing); 7,526 signatures/s (one signature 2.44 ms); the host CPU, `openssl speed -multi 4`: 5,448/s |
| compression | lz4 300, lz4x4 297, lz4x8 281, gzip 300, gzip8 249, gzip_x4 244, gunzip 278, gzip_combo 258 MHz | 28 OK, 0 FAIL, 2 SKIP: zeros through the decompressor -- a known limitation of AMD's `gzip_decompress_mm`: for a stream compressed more than roughly 100-300:1 (`gzip -6` of zeros is ~1000:1) it returns 0 bytes and its host reports success; every output checked with `lz4 -d` / `gzip -d` and compared. Compress, text: lz4 572 / lz4x4 1115 / lz4x8 1995 MB/s, gzip 588 / gzip8 1880 / gzip_x4 1816 / gzip_combo 1959 MB/s; decompress `gzip -6` text: gunzip 1172 MB/s |
| figz | gzip_figz 205 MHz | `tools/figz/test_figz.sh`: 48 OK, 0 FAIL |
| suite | (per example) | 20 / 20 TEST PASSED with AMD's C++ hosts (`examples/suite/run.sh`) and 20 / 20 PASS with the Python hosts (`examples/suite/py/run_py.sh`) on the same xclbins; built by the suite, not in `xclbin/` |

## Licences

Apache-2.0 for our work; see `NOTICE` for the third-party code (ZUC AFU: BSD-2-Clause; memcached pipeline: BSD-3-Clause;
hls4ml templates: Apache-2.0; the jet test vectors: CC-BY-4.0) and what the xclbins contain. `REUSE.toml` and the SPDX
headers cover every file (`reuse lint` passes).
