# jpegDec_fix -- AMD's jpegDec demo with a dataflow fix (Vitis_Libraries v2023.2_update1)

As released, the jpegDec hardware kernel writes `axi_height[0..2]` and `axi_width[0..2]` (infos words 11-13, 21-23)
as 0; software emulation of the same source writes 66/33/33 and 78/39/39 for images/t0.jpg, and the other fields
agree. The host then divides by `axi_width[1]` while writing the YUV file (SIGFPE after "Test passed"). See
work/docs/amd_ip_bugs.md V14 in the innova2 project.

First hypothesis (falsified, see below): `kernelJpegDecoderTop` passes the `bas_info` struct by value into
`decoder_jpg_full_top`, a `#pragma HLS DATAFLOW` region that reads `bas_info.all_blocks` directly in a call argument
(non-canonical: HLS 214-113 at that call), and then `copyInfo` reads `bas_info` again. HLS can turn a struct that enters a
dataflow region into channels, and the later reader then gets nothing for the fields it treats as consumed.

Result of the first change (2026-10-16): the decoded image (t0.raw) is byte-identical to software emulation, but words
11-13 / 21-23 were still not written -- they now held stale buffer memory (387749.., counting with the index) instead of
0, so the host crashed in the YUV loop instead. The dataflow hand-off is not the cause; the two pipelined `copyInfo`
loops over the `uint16_t` arrays `axi_height` / `axi_width` produce no writes on hardware (the neighbouring loops over
`axi_map_row2cmp` and `hls_mbs` do). Second change: those two loops become six explicit writes.

Result of the second change: all of infos matches software emulation, the host completes, and both its outputs
(t0.raw, t0.yuv) are byte-identical to software emulation. The first change was not needed; it is kept (harmless,
and removing it would need another build to re-verify).

Changes (innova2), all in `kernel/XAcc_idct.hpp` (a copy of `codec/L2/include/hw/jpegDec/XAcc_idct.hpp`; the kernel's
quoted `#include "XAcc_idct.hpp"` finds it first):
- `decoder_jpg_full_top` takes `all_blocks` as a scalar argument and passes it to `burstWrite`;
- `kernelJpegDecoderTop` gives the dataflow region its own copy, `bas_info_df`, and keeps `bas_info` for `copyInfo`;
- `copyInfo` writes `axi_height[0..2]` and `axi_width[0..2]` one by one instead of in two pipelined loops.
The Makefile's paths say `jpegDec_fix` instead of `jpegDec`; everything else is AMD's (Apache-2.0; `README_AMD.md`).
