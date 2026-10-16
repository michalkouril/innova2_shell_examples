# aes256CbcDecrypt_4pu -- AMD's 4-PU AES-256-CBC decrypt benchmark (Vitis_Libraries v2020.2)

The published aes256CbcDecrypt figure (286 MHz, 203,595 LUT, 761 BRAM, 4.7 GB/s on 32K messages of 1 KB; "1 kernel,
4 PUs") is still in the v2023.2 README, but it was measured on the v2020.x kernel. From v2021.1 on, the benchmark is a
single 128-bit CBC engine (~33K LUT; 0.96 GB/s on the Innova-2 at 200 MHz). This directory brings the 4-PU kernel back
so the published number can be compared like for like.

Sources (Apache-2.0, Copyright Xilinx, Inc.):
- `kernel/aes256CbcDecryptKernel.cpp`, `host/main.cpp`: Vitis_Libraries tag `v2020.2_rel`,
  `security/L1/benchmarks/aes256CbcDecrypt/`. The kernel is unchanged; it builds against the v2023.2 library
  (`xf::security::internal::aesCbcDecrypt` kept its interface).
- `Makefile`, `utils.mk`, `conn_*.cfg`, `xrt.cfg`, `description.json`: tag `v2023.2_update1`, same directory (the build
  flow of the other benchmarks here).

Changes (innova2):
- Makefile: kernel clock 300 MHz (as in v2020.2; v2023.2 uses 200), host links `-lcrypto` (the host computes its
  goldens with OpenSSL; needs libssl-dev on the card's host), run arguments `-mode fpga -xclbin ...` (the v2020.2 host's).
- host/main.cpp: `N_TASK` defaults to 8192 (was 2; the comment said 8192), i.e. 4 channels x 8192 tasks = 32K
  messages of 1 KB per run, overridable with `-D N_TASK=`; after the run it prints each run's kernel time (profiling
  events) and throughput. The host is built without USE_DDR: buffers are placed by kernel argument (bank0 here).
- host/main.cpp also includes `<algorithm>` (it uses `std::find`; GCC >= 11 no longer pulls it in).
- host/main.cpp: the OpenSSL golden is computed without padding (`EVP_CIPHER_CTX_set_padding(ctx, 0)`). As released,
  the golden of the last 16-byte block of every message is uninitialised memory (EVP_DecryptUpdate keeps the last
  block back for the padding check; EVP_DecryptFinal_ex fails on unpadded data and writes nothing), so every message
  "fails" at block 63. The card's block 63 equals `openssl enc -d -aes-256-cbc -nopad`.
- description.json: the v2020.2 host's arguments; no data files.
