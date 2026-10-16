# zstd_decompress_fix -- AMD's zstd_decompress demo with three decoder fixes (Vitis_Libraries v2023.2_update1)

As released, the decompress CU hangs on streams from `zstd -19 --zstd=wlog=15` (the 32 KB window this build
supports), and the CU stays stuck until another xclbin is loaded; `zstd -3` streams of the same text decompress. See
work/docs/amd_ip_bugs.md V17 in the innova2 project. All three defects reproduce in C simulation of the kernel
(`xilZstdDecompressStream` fed the .zst file), so they are in the source, not in synthesis or the platform:

1. **Repeat_Mode FSE tables** (`decodeSequence`, `L1/include/hw/zstd_decompress.hpp:176`): the table parser sends
   `0xFFFFFFFF` for "reuse the previous table" (zstd Repeat_Mode, which level 19 uses in most blocks), but the decoder
   reads the count into a `uint16_t`, so `fseVCnt != 0xFFFFFFFF` is always true; it then reads 65535 table words that
   never come (the hang; in C simulation: a stack overflow of `fseTableLL[512]` and a crash).
2. **Literals of a block with no literals** (`alignLiterals`, same file, line 116): after each block it reads one more
   word, the end-of-literals marker. In a block with zero literals the read before the loop already took that marker,
   so the extra read takes the next block's first literal word; from there literals are shifted and the CU runs out
   of input at the end (the hang). Level 19 emits such blocks (one long match, no literals) where level 3 emits two
   raw literals.
3. **Offset equal to the window size** (`L2/src/zstd_decompress_stream.cpp`): zstd allows an offset of exactly the
   window size, and `zstd -9` / `-19` emit offset 32768 for a 32 KB window, but the LZ history buffer is exactly
   32 KB, so that byte has already been overwritten (silently wrong output, no hang).

Changes (innova2):
- `kernel/zstd_decompress.hpp` (a copy of `data_compression/L1/include/hw/zstd_decompress.hpp`, found first through
  `-I $(CUR_DIR)/kernel`): `uint32_t fseVCnt`; `alignLiterals` reads the end marker only if it is still pending
  (`if (inValue.strobe)`).
- `kernel/zstd_decompress_stream.cpp` (a copy of `data_compression/L2/src/zstd_decompress_stream.cpp`): LZ history
  `2 * c_windowSize`.
- `Makefile`: compiles `kernel/zstd_decompress_stream.cpp`, `-I $(CUR_DIR)/kernel` first.
Everything else is AMD's (Apache-2.0; `README_AMD.rst`).

C simulation (2026-10-17), all byte-exact after the change; before it: crash (level 9 / 19) or byte-exact (level 1 / 3):
32 MB text at levels 1 / 9 / 19, 1 MB and 4 MB text at level 19, 2 MB random at level 19, 3 MB mixed (runs, random,
words) at levels 3 / 19; all `--zstd=wlog=15`.
