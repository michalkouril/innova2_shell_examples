# figz — gzip on the Innova-2 FPGA

`figz` is used like `gzip` (`figz file`, `figz -d file.gz`, `figz -c`, `figz -k`, `-t`, `-f`, `-v`, stdin/stdout,
`-S`, `-1`…`-9` accepted). It compresses on the card's 8-core GZip compressor and decompresses on its four
decompressors, all in one xclbin (`examples/compression/gzip_figz`; `gzip_combo`, with one decompressor, also works),
through XRT's Python bindings — no build step. Its output is ordinary gzip: `gzip -d` reads it, and `figz -d` reads any
gzip file.

```
export FIGZ_XCLBIN=<repo>/xclbin/gzip_figz.xclbin       # default: <repo>/xclbin/gzip_figz.xclbin, else gzip_combo
tools/figz/figz -v big.log                               # -> big.log.gz
tools/figz/figz -dv big.log.gz
tools/figz/test_figz.sh                                  # 48 checks against stock gzip
```

Needs XRT 2.27 with its Python bindings (`/opt/xilinx/xrt/python`, python3.14 on our hosts), the card running inn2x
r10 (found by itself; `FIGZ_DEVICE=<user-PF BDF>` with several cards), and read/write access to the card (the
render node; root works).

## How it works

* **Card:** opens the Innova-2, registers the xclbin and creates a hardware context — which loads it only if it is
  not already on the card. If the card is busy with another xclbin, or absent, figz says so and uses the CPU (zlib).
  `--cpu` forces that.
* **Compression:** the input is read in chunks (`--chunk`, default 8 MB) straight into the card's buffers; each
  chunk becomes one complete gzip member (RFC 1952 allows any number; gzip joins them). The card compresses one chunk
  while figz reads the next and writes the previous. Each member's header carries an extra field `FZ` (FEXTRA, which
  gzip ignores) holding the length of the member's compressed data. A chunk's last 32 KB block of 256 bytes or less
  is not given to the card: figz appends it as a stored block (the Vitis kernel hangs on a last block of 65-68 bytes
  and corrupts stored ones of 9-64), and an input of 256 bytes or less goes to zlib.
* **Compressor:** the Vitis 8-core GZip kernel with `patches/gzip_hist.patch`: each core still compresses its own
  32 KB block, but sees the 8 KB before it as history, and matches are extended over offsets up to 32 KB.
* **Decompression:** `FZ` lets figz cut the file into members without inflating it; each goes to the card with its
  header rewritten to the minimal form the kernel parses (it understands only a bare header or one with a file name),
  two members in flight. Every member's output is checked against its CRC32 and length; a member that fails is
  inflated on the CPU instead. That covers the kernel's one known gap: it returns no data for very highly compressed
  members (above roughly 100:1, e.g. `gzip -6` of zeros). Files from other tools: a single-member file goes to the
  card the same way; multi-member files and anything else are inflated on the CPU.
* File handling as gzip: the input is removed unless `-k` / `-c`, mode and times are kept, existing outputs are not
  overwritten without `-f`, compressed data is not written to a terminal without `-f`.

## Measured (HP Z440: Xeon E5-1603 v4, 4 cores; whole command through the resident helper, output to /dev/null)

| | ratio: docs / python / binaries / syslog | compress 419 MB text | decompress it |
|---|---|---|---|
| **`figz`** (`gzip_figz`, history, 205 MHz) | **5.06× / 3.88× / 2.32× / 6.85×** | **0.97 GB/s** | **0.88 GB/s** |
| `figz` before the history patch (230 MHz) | 4.55× / 3.71× / 2.26× / 6.30× | 1.23 GB/s | 0.86 GB/s |
| `gzip -1` | 4.90× / 3.69× / 2.26× / 6.68× | 0.12 GB/s | `gzip -d`: 0.42 GB/s |
| `gzip -3` | 5.29× / 4.02× / 2.34× / 7.05× | | |
| `gzip -6` (default) | 6.41× / 4.58× / 2.45× / 8.16× | | |

So figz compresses a little better than `gzip -1`, between it and `gzip -3`, at about 8× its speed, and decompresses
its own files at about 2× `gzip -d` (four decompressors, one member each). A file made by gzip is a single member,
decompressed on one decompressor: about as fast as `gzip -d` on text, slower on incompressible data. The output is
valid gzip but not byte-identical to gzip's. `-1`…`-9` are accepted and ignored: the card has one level.
