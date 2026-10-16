# FLD-lite and E-series examples (ConnectX-5 driven from the FPGA, no CPU in the data path)

Kernels and host programs from the E1 - E5 experiments on the Innova-2 dev platform. The FPGA polls the ConnectX-5's
completion queues and rings its doorbells itself (through the PEER window); the host only creates the queues (DevX).
Write-up of the throughput / latency work: see the repository history and `e5/fld_lite.cpp` comments.

| dir | what |
|---|---|
| `e1` | DevX send queue helpers (`devx_sq.*`), `prm.h` shim, first FPGA-rung doorbell test |
| `e2` | DevX receive queue / TIR helpers (`devx_rx.*`), RX experiments |
| `e3` | DevX CQ helpers (`devx_cq.*`), FPGA echo kernels (`e3_echo*`), host-echo baseline, `e5_ticker` |
| `e5` | **FLD-lite** (`fld_lite.cpp`: `fld_rx`, `afu_loop`, `fld_tx`, `fld_reap`, `e5_ticker2`), the FPGA traffic generator / sink (`gen/gen_sink.cpp`), hosts `e5_host.cpp` (latency / bursts) and `e5gen_host.cpp` (generator sweeps, ZUC), threaded software simulator `sim/` |
| `e5/zuc` | 4-core ZUC (128-EEA3/EIA3) AFU from FlexDriver (`rtl4/`, BSD-2-Clause, (c) Gabi Malka) packaged as a Vitis RTL kernel, adapters `zuc_adapt.cpp`, model and RTL testbenches |
| `innova2_peermem` | peer-memory client kmod: lets the ConnectX (RDMA) reach FPGA DDR (GPL-2.0-only) |
| `lat` | `lat_probe` kernels: kernel-to-memory round-trip latency per memory type |
| `hostmem` | tier-1 host memory access: `innova2_hostmem.ko` (1 MB DMA-coherent host buffers) + `hostmem_probe` (sp=peer) -- an RP kernel reads/writes host RAM through peer_window windows 2/3 (hpy440: 6.99 / 6.95 GB/s, 1.03 us dependent read) |

## Requirements

See the repository's README: a card running inn2x r10 (platform `innova2_innova2_xdma_202320_4`), OFED with DevX,
P2P enabled, and for the generator tests port 1 of the ConnectX looped to port 0 (DAC). `host_setup.sh` builds the
host programs and `innova2_peermem.ko`; it copies `mlx5_ifc.h` / `mlx5_ifc_fpga.h` (dual GPL-2.0 / OpenIB BSD, not
committed here) from `/usr/src/ofa_kernel/<arch>/<kver>/include/linux/mlx5/`. The run scripts switch the card to HYBRID
DDR mode (rings and RX slots live in the 1 MB on-chip window). The maintained builds and runs are in `../examples/`
(`fld_echo`, `zuc`, `memcached`, `hls4ml`, `lat_probe`, `hostmem`); the notes below are the original E-series sketch.
`hostmem`'s XRT HOST[0] variant needs the xocl patch shipped with the shell release (`innova2_shell` `tools/xrt/`).

## Build and run (sketch)

```
v++ -c -t hw --platform $PLATFORM -k fld_rx -o fld_rx.xo e5/fld_lite.cpp      # likewise afu_loop fld_tx fld_reap e5_ticker2
v++ -c -t hw --platform $PLATFORM -k gen_tx -o gen_tx.xo e5/gen/gen_sink.cpp   # and sink_rx
cp e5/link_fld_gen.cfg link.cfg; PLATFORM=... e5/link.sh . fld.xclbin
make -C e5 -f Makefile.host e5gen_host e5_host
sudo ./e5gen_host <user BDF> fld.xclbin mlx5_0 mlx5_1 <cx5 BDF> 200000 --len 256 --gap 0 --bf --inline 222 --inline-adapt --rxk 4 --sample 4
sudo ./e5_host    <user BDF> fld.xclbin mlx5_0 mlx5_1 <cx5 BDF> 2000 --lat --len 64 --bf --inline 222 --pre 1026
```

Always sweep with `--sample 4` (1 frame in 16 byte-checked): checking every frame makes `sink_rx` the bottleneck.

Knobs (all encoded in existing kernel arguments, so older hosts keep working):
`fld_rx pre = npre | K << 8 | fixedK << 16` (CQEs per poll; adaptive by default), `fld_tx inline_max = cap | adaptive << 16`,
`fld_tx maxb = batch | linger << 8`.

## Measured (hpy440, fp4 shell, 300 MHz, 10 GbE)

| frame | one CQE per poll | K=4 burst polling |
|---|---|---|
| 60 B | 1.49 M frames/s | 2.46 M |
| 256 B | 0.74 M | 2.36 - 2.57 M, lossless |
| 1024 / 1500 B | 0.69 / 0.65 M | 10 GbE line rate, lossless |

Idle 64 B echo, ConnectX RX CQE to TX CQE: 1.86 us (K=1, inline). ZUC (4 cores, 195 MHz): 9.5 - 9.8 Gb/s, 0 errors.
