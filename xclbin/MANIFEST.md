# xclbins

Built 2026-10-16 by `build_all.sh -j 2` from this tree (`ntp.xclbin`: 2026-10-17, `examples/ntp/build.sh`; `iperf.xclbin` / `iperf_client.xclbin`: 2026-10-17, `examples/iperf/build.sh` / `build_client.sh`) against platform `innova2_innova2_xdma_202320_4` (inn2x r10 dev
platform), Vitis 2023.2; tested on a card the same day (README, "Results"). Check them with `sha256sum -c
xclbin/SHA256SUMS` from the repository root. Data clock = the kernel clock v++ selected. Licences: `REUSE.toml` and
`NOTICE` (our kernels, the third-party kernel code, and the AMD IP of the platform's dynamic region, LicenseRef-AMD-IP).

| xclbin | source | data clock | size | sha256 (first 16) |
|---|---|---|---|---|
| `fld_echo.xclbin` | `examples/fld_echo` | 300 MHz | 33.7 MB | `bb10022270171a6b` |
| `gunzip.xclbin` | `examples/compression/gunzip` | 278 MHz | 28.1 MB | `3e8e4895b3f334dc` |
| `gzip.xclbin` | `examples/compression/gzip` | 300 MHz | 27.1 MB | `2d33b5357ebe4dee` |
| `gzip8.xclbin` | `examples/compression/gzip8` | 249 MHz | 34.1 MB | `ef436f836ca467b7` |
| `gzip_combo.xclbin` | `examples/compression/gzip_combo` | 258 MHz | 35.0 MB | `fcfe8b81143c3da1` |
| `gzip_figz.xclbin` | `examples/compression/gzip_figz` | 205 MHz | 35.8 MB | `03cf2535ba884d7e` |
| `gzip_x4.xclbin` | `examples/compression/gzip_x4` | 244 MHz | 35.7 MB | `140e0fbc7f600c99` |
| `hls4ml.xclbin` | `examples/hls4ml` | 241 MHz | 32.0 MB | `bb5b374dfd36905c` |
| `hostmem.xclbin` | `examples/hostmem` | 300 MHz | 27.8 MB | `0744bdb7fd9fc704` |
| `lat_probe.xclbin` | `examples/lat_probe` | 300 MHz | 26.2 MB | `d92fbca8cabc57fb` |
| `lz4.xclbin` | `examples/compression/lz4` | 300 MHz | 28.4 MB | `19e583525cf58c45` |
| `lz4x4.xclbin` | `examples/compression/lz4x4` | 297 MHz | 30.8 MB | `5f82fa94caebadf2` |
| `lz4x8.xclbin` | `examples/compression/lz4x8` | 281 MHz | 32.7 MB | `2b738b4ae8358fca` |
| `memcached.xclbin` | `examples/memcached` | 300 MHz | 31.1 MB | `b122af89e432e86f` |
| `iperf.xclbin` | `examples/iperf` | 300 MHz | 30.1 MB | `0e5a26f7c52fb49e` |
| `iperf_client.xclbin` | `examples/iperf` (`build_client.sh`) | 300 MHz (fns kernels: clock id 4) | 29.9 MB | `8d9024f65cb81ef7` |
| `ntp.xclbin` | `examples/ntp` | 300 MHz | 29.6 MB | `99bb07adb1beec1c` |
| `rsa.xclbin` | `examples/rsa` | 213 MHz | 35.7 MB | `ad88946313cf9ac9` |
| `zuc.xclbin` | `examples/zuc` | 227 MHz | 36.6 MB | `782706d3f7602fb9` |
