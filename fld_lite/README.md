# FLD-lite: the ConnectX-5 driven from the FPGA, no CPU in the data path

FLD-lite and the kernels and host programs around it. The FPGA polls the ConnectX-5's
completion queues and rings its doorbells itself (through the PEER window); the host only creates the queues (DevX).
The design notes are in `e5/fld_lite.cpp`'s comments.

| dir | what |
|---|---|
| `e5` | **FLD-lite** (`fld_lite.cpp`: `fld_rx`, `afu_loop`, `fld_tx`, `fld_reap`, `e5_ticker2`), the UDP layer (`udp_lite.cpp`, `udp_swap.cpp`), the FPGA traffic generator / sink (`gen/gen_sink.cpp`), the host programs (`e5_host` latency, `e5gen_host` generator sweeps and ZUC, `e5udp_host` the UDP endpoint memcached / ntp / hls4ml run on), a threaded software simulator (`sim/`) |
| `e5/zuc` | 4-core ZUC (128-EEA3/EIA3) AFU from FlexDriver (`rtl4/`, BSD-2-Clause, (c) Gabi Malka) packaged as a Vitis RTL kernel, adapters `zuc_adapt.cpp`, a C model, and the RTL testbench `sim_rtl/` |
| `devx` | the DevX helpers the host programs create the ConnectX queues with: send queue (`devx_sq`), receive queue / TIR (`devx_rx`, and `devx_rx_s0` with the UDP steering rule), completion queue (`devx_cq`), the `prm.h` shim for OFED's `mlx5_ifc.h` |
| `innova2_peermem` | peer-memory client kmod: lets the ConnectX (RDMA) reach FPGA DDR (GPL-2.0-only) |
| `lat` | `lat_probe` kernels: kernel-to-memory round-trip latency per memory type |
| `hostmem` | tier-1 host memory access: `innova2_hostmem.ko` (1 MB DMA-coherent host buffers) + `hostmem_probe` (sp=peer) -- an RP kernel reads/writes host RAM through peer_window windows 2/3 |

## Requirements

See the repository's README: a card running inn2x r10 (platform `innova2_innova2_xdma_202320_4`), OFED with DevX,
P2P enabled, and for the generator tests port 1 of the ConnectX looped to port 0 (DAC). `host_setup.sh` builds the
host programs and `innova2_peermem.ko`; it copies `mlx5_ifc.h` / `mlx5_ifc_fpga.h` (dual GPL-2.0 / OpenIB BSD, not
committed here) from `/usr/src/ofa_kernel/<arch>/<kver>/include/linux/mlx5/`. The run scripts switch the card to HYBRID
DDR mode (rings and RX slots live in the 1 MB on-chip window). The maintained builds and runs are in `../examples/`
(`fld_echo`, `zuc`, `memcached`, `ntp`, `hls4ml`, `lat_probe`, `hostmem`).
`hostmem`'s XRT HOST[0] variant needs the xocl patch shipped with the shell release (`innova2_shell` `tools/xrt/`).

## Knobs

`examples/fld_echo/run.sh` and `examples/zuc/run.sh` show the host programs' options. The FLD-lite knobs are encoded in
existing kernel arguments, so older hosts keep working: `fld_rx pre = npre | K << 8 | fixedK << 16` (CQEs per poll;
adaptive by default), `fld_tx inline_max = cap | adaptive << 16`, `fld_tx maxb = batch | linger << 8`. Sweep with
`--sample 4` (1 frame in 16 byte-checked): checking every frame makes `sink_rx` the bottleneck.

## Measured (HP Z440, Xeon E5-1603 v4; inn2x r10, 300 MHz, 25 GbE DAC; 2026-10-17)

`examples/fld_echo/run.sh`: the FPGA generator on port 1, FLD-lite echoing on port 0, 200,000 frames per size at gap 0,
K = 4 burst polling, 0 TX / RX errors and 0 mismatches:

| frame | echoed by FLD-lite |
|---|---|
| 64 B | 2.75 M frames/s |
| 256 B | 1.64 M frames/s |
| 512 B | 1.57 M frames/s (6.4 Gb/s) |
| 1024 / 1500 B | 9.94 / 9.42 Gb/s of frames, lossless (1.21 / 0.79 M frames/s) |

Idle 64 B echo: host round trip 4.29 us, of which ConnectX RX CQE to TX CQE in the FPGA 1.87 us. ZUC
(`examples/zuc`, 4 cores at 227 MHz): integrity 19.45 Gb/s, confidentiality 9.67 Gb/s on 8191 B frames, 0 errors.
Host memory (`examples/hostmem`): 6.97 GB/s write, 7.43 GB/s read, 1.05 us dependent read.
