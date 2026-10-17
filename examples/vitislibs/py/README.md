<!--
SPDX-FileCopyrightText: 2026 the innova2 contributors
SPDX-License-Identifier: Apache-2.0
-->
# Python (pyxrt) hosts for the Vitis Libraries benchmarks

One script per benchmark of `../benchmarks.txt` (`<basename of the benchmark path>.py`), replacing AMD's C++ host: no
Vitis_Libraries checkout, no compiler, no AMD data files on the card's host -- only the xclbin, python3, numpy, XRT's
pyxrt (`source /opt/xilinx/xrt/setup.sh`), plus `cryptography` (AES) and the `lz4` / `zstd` CLIs (compression).
Each one does what AMD's host checks (and the checks our README added), at the sizes the README results used, and
ends with a `PYHOST-PASS` / `PYHOST-FAIL` line. Inputs are generated (seeded) by default; `--data` / `--input` /
`--jpeg` take AMD's files or the SuiteSparse files of `../graphdata/prepare.sh` (`-h` on each script).

```
python3 crc32.py <xclbin>                 # one benchmark
bash run_py.sh <root>                     # all of them: <root>/Vitis_Libraries/<benchmark>/build_dir.hw.*/*.xclbin
```

Times are host-measured, start to completion of one run (so they include ~10-50 us of XRT dispatch); AMD's C++ hosts
report OpenCL profiling times. Verified on hpy440 (Xeon E5-1603 v4, Ubuntu 26.04, XRT 2.27, python 3.14, inn2x r10),
2026-10-17, with the same xclbins the C++ hosts ran (byte-identical): `run_py.sh` 21/21 PASS.

## Security, compression, database, codec

hpy440 (Xeon E5-1603 v4, Ubuntu 26.04, XRT 2.27, python 3.14, inn2x r10), 2026-10-17, the prebuilt xclbins of
`examples/vitislibs` (byte-identical to the ones the C++ hosts ran, README "Results"). `run_py.sh` on all twelve:
12/12 PASS. Times are host-measured (start to completion of the run, so they include ~10-30 us of XRT dispatch);
AMD's C++ hosts report OpenCL profiling times.

| benchmark | Python host | C++ host / README | check |
|---|---|---|---|
| crc32, 32768 x 8 KB | 4.66 GB/s | 4.67 GB/s | every CRC vs zlib; AMD's test.dat gives AMD's golden 0xff7e73d8 |
| adler32, 32768 x 8 KB | 4.61 GB/s | 4.61 GB/s | every checksum vs zlib |
| aes256CbcDecrypt, 32K x 1 KB | 1.00 GB/s | 0.96 GB/s | every message vs the plaintext (ciphertext made with `cryptography`) |
| aes256CbcDecrypt_4pu, 4 x 8192 x 1 KB | 5.3-5.5 GB/s | 3.65 GB/s | every lane vs the golden; see note 1 |
| jpegDec_fix, t0.jpg | 0.82 ms (10 runs) | 0.98 ms | info words + every block vs a Python decode: max diff 1, 67.9 dB |
| resize, 7680x4320 -> 512x512 / 1080p / 2160p | 15.5 ms each | 15.5 ms | every pixel vs a bit-exact numpy model: 0 differ |
| zstd_decompress, level 3, 32 MB text | 952 MB/s | 952 MB/s | byte-exact; see note 2 |
| zstd_decompress_fix, level 19 (1 / 9 / 19) | 949 (971 / 1150 / 949) MB/s | 912 (966 / 1149 / 912) | byte-exact; note 2 |
| lz4_streaming, 32 MB text | 198-206 MB/s compress, 1478-1481 MB/s decompress | 212 / 1482 MB/s | `lz4 -d` of the FPGA frame and the FPGA round trip byte-exact; note 3 |
| snappy_streaming, 4 MB in 4 MB blocks | 285-295 MB/s compress | 285 MB/s | Python decode of the FPGA stream; FPGA round trip on 64 KB only (V18); note 4 |
| snappy_streaming_fix, 32 MB in 64 KB blocks | 213-219 MB/s compress, 1788 MB/s decompress | 222 / 1790 MB/s | Python decode, FPGA round trip, FPGA decode of a Python-encoded 4 MB stream |
| compound_sort, 131072 keys | 448-457 MiB/s (best of 5) | 439 MiB/s | vs numpy's sort |

Checks beyond AMD's hosts: resize (AMD's host compares nothing: a numpy model of `resizeDown_opt_8x` in
`ap_fixed<33,14>`, validated bit-exact against the earlier software-emulation outputs too), jpegDec (a baseline JPEG
decode in Python; against the earlier C-model output: max diff 1 on 5173 of 494208 samples, the float vs integer
IDCT), the compression formats (the stock `lz4` CLI, a pure-Python snappy decoder and encoder).

Notes:

1. aes256CbcDecrypt_4pu is faster here because AMD's host overlaps the next buffer's host-to-card DMA with the
   kernel (ping-pong) on the card's single DDR4 bank; the Python host runs the two buffers one after the other, so the
   kernel has the bank to itself. The README's 3.65 GB/s is the overlapped figure.
2. **New finding (AMD kernel, both builds): the zstd decompressor decodes one stream per xclbin load.** After a
   stream has been decoded, the next one hangs the CU (until another xclbin is loaded), whichever host runs it:
   stock xclbin with AMD's C++ host `xil_zstd`, level 3 then level 3 -> the second times out (rc 124); fix xclbin:
   py/py, C++/C++, C++/py and py/C++ all hang on the second stream. With another xclbin loaded before every stream,
   all of levels 1 / 3 / 9 / 19, with and without the frame content size, decode byte-exact. The earlier results were single runs, so never saw it. The ports
   decode one stream per invocation; `--levels 1,9,19 --reset-xclbin <other xclbin>` runs several (each in a child
   process after loading the other xclbin). The input is piped into zstd with `--stream-size` (content size in the
   frame, as a file-based run) -- not needed (the matrix passes both ways), only to match file-made streams.
3. lz4 compression is block by block (512 launches of two kernels for 32 MB); Python's per-launch cost shows as
   ~3-7 % less throughput than the C++ host.
4. **New finding (AMD snappy compressor): with blocks of 4 MB (`-B 3`, the README's 285 MB/s configuration) every
   compressed block's uncompressed-length preamble is wrong**: `80 80 00` (= 0) instead of `80 80 80 02` (4194304),
   i.e. the varint is cut to three bytes. AMD's own decompressor ignores the preamble, standard snappy decoders
   reject the stream. 64 KB blocks are correct (`80 80 04`); 256 KB / 1 MB not tested. The port reports it and fails
   only with `--strict`; the data itself decodes correctly.

Inputs: generated (seeded) or the host's `/usr/share/doc/*/copyright` texts repeated to the size (as
`examples/compression/run.sh`); jpegDec uses AMD's images in `../overlay/codec/L2/demos/jpegDec_fix/images/`;
`--data` / `--input` / `--jpeg` take files. Dependencies: python3, numpy, pyxrt (XRT), `cryptography` (AES), the
`lz4` and `zstd` CLIs.

## Graph, quantitative finance, solver

Card: hpy440, inn2x r10, XRT 2.27, python 3.14 + numpy (nothing else). xclbins: the ones the C++ hosts ran with
(`Vitis_Libraries/<benchmark>/build_dir.hw.innova2_innova2_xdma_202320_4/`). Timing is the
host-measured start-to-completion of one run (pyxrt), so it includes XRT's dispatch (~10-50 us); the C++ figures are
the README's (OpenCL profiling events or gettimeofday around the enqueue).

| benchmark | input | Python host | C++ host (README) |
|---|---|---|---|
| shortest_path_float_pred | soc-LiveJournal1 (`--data`, golden) | PASS, 7.764 s, 4,400,347 reachable, 0 distance / 0 predecessor errors | PASS, 7.76 s |
| | AMD bundled (`--data`) | PASS | PASS |
| | random simple graph, 20,000 V / 200,000 E, weights (0, 1] (default) | PASS, 0.17 s, Dijkstra on the host | — |
| triangle_count | europe_osm (`--data`, golden 61,710) | PASS, 1.732 s, 61,710 | PASS, 1.73 s |
| | AMD bundled | PASS, 11 | PASS |
| | random, 20,000 V / 200,000 E (default) | PASS, 13 ms, 1,334 (exact host count) | — |
| label_propagation | hollywood-2009, 1 iteration (`--data`, lp_csim golden) | PASS, 42.80 s, 0 of 1,139,905 labels differ, most-frequent check 0 bad | PASS, 42.8 s |
| | hollywood-2009, 4 iterations | PASS, 53.35 s, 0 differ | PASS, 53.4 s |
| | AMD bundled | PASS, golden identical | PASS |
| | random, 20,000 V / 202,000 E, 1 iteration (default) | PASS, 0.30 s, most-frequent check 0 bad | — |
| twoHop | soc-LiveJournal1, 1000 pairs (`--data`, golden) | PASS, 9.27 ms, 0 wrong | PASS, 9.15 ms |
| | AMD bundled | PASS, 3.6 ms | PASS |
| | random, 20,000 V / 200,000 E, 1000 pairs (default) | PASS, 4.4 ms, exact host counts | — |
| maximal_independent_set | AMD bundled (`--data`) | PASS, 1.26 ms, 212 vertices = mis.txt | PASS, 1.22 ms, = golden |
| | random 1015 V / 11,000 entries (default) | PASS, 1.05 ms, independent + maximal | — |
| SVD (4x4) | AMD's matrix | PASS, cold 158 us, warm 49 us, err 8.8e-11 | PASS, 197 us cold |
| Quadrature | AMD's vector | PASS, 1.04 ms, max err 5.9e-5 | PASS, 1.12 ms |
| | + 20 random contracts (`--extra 20`) | PASS, 0 out of tolerance, max err 9.1e-5 | — |
| gesvdj (16x16) | random symmetric (seed 12) | PASS, 0.61 ms first run, err 3.4e-11 | PASS, 0.64 ms |
| gtsv (16x16) | AMD's system (`--random S`: random) | PASS, 0.13-0.14 ms first run, err 1.1e-16 | PASS, 0.27 ms |

### Findings while porting

* **shortest_path_float_pred: weighted multigraphs.** With duplicate edges and self-loops (`--multigraph`) and
  non-unit weights, 165 of 20,000 distances came out longer than the shortest path (up to +82 %), each consistent with
  the kernel's own predecessor path, and no overflow flag set. Simple graphs with random weights, and any graph with
  unit weights, are correct. The default synthetic graph is simple.
* **maximal_independent_set: the write position persists.** `count_S` is a static register: a second run without
  reloading the xclbin writes its result after the first's (seen: `res_out[232]`). AMD's host compares from index 0,
  so it would fail a second run in the same xclbin load. The Python host finds the span the kernel wrote and sizes
  `res_out` as AMD does (M_VERTEX), so repeated runs cannot overrun it.
* **label_propagation: the update rule.** Iteration 1 = every vertex takes a most frequent label among its in- and
  out-neighbours, a self-loop counted once (verified on all of hollywood-2009); later iterations do not follow that
  rule from the other ping/pong buffer or from a replay with N - 1 iterations, so for N > 1 only a golden is checked.
* **gesvdj / gtsv** are compiled for 16 x 16; the ports have no size option (a bigger size hangs the kernel).
* The SuiteSparse inputs are the files `examples/vitislibs/graphdata/prepare.sh` makes, read with numpy's C text
  parser (soc-LiveJournal1: about 40 s per run including the load).
