# vitislibs — AMD Vitis Libraries benchmarks on the Innova-2

AMD's [Vitis Libraries](https://github.com/Xilinx/Vitis_Libraries) (v2023.2_update1, Apache-2.0) benchmarks, built with
their own Makefiles for this card (inn2x r10, platform `innova2_innova2_xdma_202320_4`) and compared with the numbers AMD
publishes. `benchmarks.txt` lists the ones that build and pass here, and — commented — the ones that do not, with why.

## Use

On the build machine (Vitis 2023.2, the dev platform; see the top-level README):

```
source <Vitis 2023.2>/settings64.sh; export PLATFORM=<path>/innova2_innova2_xdma_202320_4.xpfm BUILD=<scratch dir>
VL_JOBS=2 VL_CLEAN=1 bash examples/vitislibs/build.sh          # every active line of benchmarks.txt; or name some
```

`build.sh` fetches the library (sparse: no docs/tests/data except what a benchmark needs), restores each benchmark,
pins every memory port to bank0 (`pin_cfg.py`, `SYS_PORT` in the sources, and `sp_fill.py` for ports a cfg leaves out),
compiles the kernels, refuses to link when they need more URAM / LUTs than the region has left (`uram_check.py`), links,
and checks the link (`common/link_gate.sh`, every port on bank0). Logs: `$BUILD/vitislibs/<benchmark>.log`.
`VL_CLEAN=1` keeps the reports (`<benchmark>/innova2_reports/`) and deletes the 1–3 GB temp dir.

On the card's host, with `$BUILD/vitislibs/Vitis_Libraries` copied over (see `build_hosts.sh`):

```
HLS_INCLUDE=<copy of Vitis_HLS/include> bash examples/vitislibs/build_hosts.sh <copy>/Vitis_Libraries
bash examples/vitislibs/run.sh <copy>/Vitis_Libraries          # PASS / FAIL per benchmark + its timing lines
```

`build_hosts.sh` builds each host there against the host's XRT (C++17), pins the hosts' Alveo bank choices to bank0
(`pin_host.py`) and applies `host_patches/` (timing printouts AMD left commented out, array sizes for AMD's data sets).
`run.sh` uses `args=` from `benchmarks.txt` where AMD's figure needs a bigger input than the default, and runs without
the benchmarks' `xrt.ini` profiling options (they crash XRT 2.27 at exit). `check_resize.sh` checks resize's output.

## Results (hpy440, Xeon E5-1603 v4, inn2x r10, XRT 2.27; 2026-10-16/17)

Clock = the data clock v++ chose (the platform asks for 300 MHz). Every run below passed its host's check; where the
host checks nothing or the default input is too small, the check named was added.

| benchmark | clock | input | Innova-2 | AMD published | |
|---|---|---|---|---|---|
| security crc32 | 300 | 256 MB | **4.67 GB/s** | 4.7 GB/s (U50, 300 MHz) | matches |
| security adler32 | 300 | 256 MB | **4.61 GB/s** | 4.1 GB/s (262 MHz) | matches |
| security aes256CbcDecrypt_4pu (overlay) | 300 | 32K × 1 KB | **3.65 GB/s** (3.40 GiB/s) | 4.7 GB/s (286 MHz) | 1 DDR4 bank for in + out, U50: separate HBM |
| security aes256CbcDecrypt (v2023.2) | 200 | 32K × 1 KB | 0.96 GB/s | — | single engine; AMD's row is the 4-PU kernel |
| codec resize, 7680×4320 → 512² / 1080p / 2160p | 272 | synthetic 8K | **15.5 ms**, output = C model | 12.4–12.6 ms (8 PPC, 341 MHz) | same per clock |
| codec jpegDec_fix (overlay), 616×516 | 288 | t0.jpg | 0.98 ms, output = C model | (image only) | stock jpegDec: see notes |
| data_compression lz4_streaming, 32 MB text | 274 | | 212 MB/s compress, 1.48 GB/s decompress | 287 MB/s, 1.8 GB/s (300 MHz) | byte-exact both ways |
| data_compression zstd_decompress, 32 MB text (zstd -3, 32 KB window) | 228 | | 952 MB/s (output) | 463 MB/s | data not stated; level 19 hangs |
| data_compression snappy_streaming | 300 | 4 MB, 4 MB blocks | 285 MB/s compress | 260 MB/s | decompress > 64 KB hangs |
| data_compression zstd_decompress_fix (overlay), 32 MB text, 32 KB window | 231 | zstd -1 / -9 / -19 | 966 / 1149 / 912 MB/s, all byte-exact | 463 MB/s | the stock kernel hangs on -19 |
| data_compression snappy_streaming_fix (overlay), 32 MB text, 64 KB blocks | 300 | | 222 MB/s compress, 1.79 GB/s decompress, round trip byte-exact | 260 MB/s compress | the stock kernel hangs > 64 KB |
| database compound_sort, 131072 keys | 271 | | **439 MB/s** | 443 MB/s (U280, 287 MHz) | matches |
| graph shortest_path_float_pred | 300 | soc-LiveJournal1 | 7.76 s | 25.94 s (U250) | weights 1.0 here, AMD's not stated |
| graph triangle_count | 300 | europe_osm | **1.73 s**, 61710 triangles | 1.08 s (U250) | one DDR4 bank |
| graph label_propagation | 272 | hollywood-2009 | 42.8 s (1 iter), 53.4 s (4 iter) | 107.39 s (iterations not stated) | |
| graph twoHop | 300 | soc-LiveJournal1, 1000 pairs | 9.15 ms | 38.9 s (pairs not published) | not comparable |
| graph maximal_independent_set | 157 | bundled | 1.22 ms, = golden | — | |
| quantitative_finance SVD 4×4 | 300 | | **197 µs** (cold) | 196 µs cold, 22 µs warm | matches |
| quantitative_finance Quadrature | 300 | | 1.12 ms, 0 out of tolerance | — | |
| solver gesvdj 16×16 | 200 | | 0.64 ms | (512×512: 128 URAM) | |
| solver gtsv 16×16 | 300 | | 0.27 ms | (1024: does not route) | |

Graph inputs (`graphdata/`: `build.sh` + `prepare.sh` make them and their references from scratch): SuiteSparse
(soc-LiveJournal1, europe_osm, hollywood-2009) converted to each host's CSR format, with
CPU references (distances, the triangle count, two-hop counts, labels per iteration from AMD's kernel code run as C++).
triangle_count and label_propagation need the larger host arrays in `host_patches/`; triangle_count takes its expected
count from `TC_GOLDEN` (default 11, the bundled graph's).

## Notes

* **Fit.** The dynamic region leaves kernels 32 URAM (the platform's own logic takes 48 of its 80), bank0 accepts 15
  masters, and kernels above ~300K LUTs failed placement. 13 benchmarks of the original list and the 9 TreeEngine
  models do not fit (`benchmarks.txt`, commented); `uram_check.py` now stops such builds before the link.
* **jpegDec** (stock): the kernel's two pipelined `copyInfo` loops over `axi_height` / `axi_width` (auto-partitioned
  `uint16_t` arrays) write nothing on hardware (software emulation writes them), and the host divides by 0. The
  `jpegDec_fix` overlay writes the six words one by one; its output then equals software emulation.
* **resize**: the host never sets `nerror`; `check_resize.sh` compares the card's output with software emulation.
* **aes256CbcDecrypt**: the README's 4.7 GB/s row is the v2020.x 4-PU kernel; `aes256CbcDecrypt_4pu` brings it back
  (and fixes its host's golden: OpenSSL padding left block 63 of every message uninitialised).
* **gesvdj / gtsv** are compiled for 16×16 and their hosts do not check `-M`: a bigger size hangs the kernel.
* **System-compiler (`_sc`) variants** build after `SYS_PORT` pinning, but Vitis 2023.2's `libvpp_acc.a` calls XRT's
  legacy `xclProbe` / `xclOpen`, which XRT ≥ 2.17 no longer exports, so their hosts cannot be linked.
* zstd_decompress (level-19 streams) and snappy_streaming (decompression > 64 KB) hang the CU until another xclbin is
  loaded; the `_fix` overlays fix both (their READMEs): zstd has three source bugs (a Repeat_Mode table-count compare
  that is never true, an extra literal read after a block with no literals, a 32 KB history for a 32 KB window), all
  reproducible in C simulation; snappy's source is correct, but the RTL HLS 2023.2 makes of its chunk-header loop starts
  every chunk after the first one word early (found in RTL co-simulation), and the overlay restructures that loop.
