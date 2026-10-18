# compression: no-compile hosts

`bash examples/compression/py/run_py.sh [variant ...]` runs the same variants, inputs and checks as `../run.sh`. It
uses pyxrt hosts in place of the Vitis Libraries C++ hosts, so it needs no Vitis_Libraries checkout, no OpenCL,
Boost or HLS headers, and no compiler.

| host | kernel | ported from |
|---|---|---|
| `lz4_host.py` | `xilLz4Compress` (lz4, lz4x4, lz4x8) | AMD `lz4OCLHost.cpp` |
| `gzip_host.py` | `xilGzipCompBlock` (gzip, gzip8, gzip_combo; gzip_x4 with 4 CUs) | `../host/gzip_xn_host.cpp` |
| `gunzip_host.py` | `xilDecompress` | AMD `decompressEngineMMSeq` |

The LZ4 frames carry no content checksum, because a pure-Python xxHash32 of 32 MB would be slow. `lz4 -d` still
checks every block.

On an HP Z440 (Xeon E5-1603 v4) with r10 both runs give 28 OK, 0 FAIL, 2 SKIP. Kernel throughput (MB/s, text input):

| variant | C host | Python host |
|---|---|---|
| lz4 | 526 | 572 |
| lz4x8 | 1546 | 1986 |
| gzip | 317 | 589 |
| gzip8 | 500 | 1893 |
| gzip_x4 | 1814 | 1772 |
| gunzip | 1166 | 1174 |

The Python hosts time one whole-file launch per CU. The stock C hosts time their own chunked flow, and for
compression that reads lower. The kernels are the same and so are the verdicts. Which part of the C hosts' interval
makes the difference was not investigated.
