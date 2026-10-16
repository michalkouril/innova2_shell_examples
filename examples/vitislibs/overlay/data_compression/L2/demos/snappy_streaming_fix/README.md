# snappy_streaming_fix -- AMD's snappy_streaming demo with a decompressor work-around (Vitis_Libraries v2023.2_update1)

As released, the decompress CU hangs as soon as a stream has a second chunk (output > 64 KB with the default 64 KB
blocks), and stays stuck until another xclbin is loaded. See work/docs/amd_ip_bugs.md V18 in the innova2 project.

The source is correct: C simulation, software emulation and a threaded C model with every FIFO bounded to its pragma
depth all decompress 128 KB / 256 KB / 3 MB (238 chunks) byte-exact. RTL co-simulation of the kernel does not: on
128 KB it stops after 65,581 bytes. Probing the RTL simulation localised it to `snappyHeaderProcessing`
(`data_compression/L1/include/hw/snappy_decompress_details.hpp`): its data loop (line 133) is synthesised with the
512-bit window as an in/out register, and its first iteration reads that register instead of the value the loop is
entered with. The outer loop (line 80) shifts the window before parsing each chunk header; from the second chunk on
that shift has not reached the register, so the chunk's first output word is the stale window (here the chunk header
bytes `00 00 00 00 00 80 80 04`) and every following word is one word late. The token decoder then reads a data byte as
a tag with the 4-byte-offset code (3), which it does not implement; it emits tokens forever without advancing (the
hang). Copying the window into a local for the loop does not help (the loop-carried register is the same).

Change (innova2), in `kernel/snappy_decompress_details.hpp` (a copy of the L1 header, found first through
`-I $(CUR_DIR)/kernel`): the outer loop no longer shifts the window, so nothing modifies it between data loops; the
data loop absorbs the bytes the chunk header adds by spending an iteration on a shift without output while its index
is still >= 8. Same output in C; RTL co-simulation of the kernel: 128 KB byte-exact.
`Makefile`: `-I $(CUR_DIR)/kernel` first. Everything else is AMD's (Apache-2.0; `README_AMD.rst`).

Not changed: the decoder (`snappyMultiByteDecompress`) reads three 8-byte words of every chunk before it looks at the
chunk's size, so a very short chunk hangs it too, in C simulation as well (a 9-byte chunk between two text chunks
hangs, a 282-byte one does not). The FPGA compressor makes such a chunk only at the end of a stream (a short tail
block), where the compressor itself already stalls on a 1-byte tail (65,537 B input; V18, not investigated).
