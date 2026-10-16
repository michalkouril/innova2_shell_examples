# gtsv_1024 -- AMD's gtsv benchmark at its published size (Vitis_Libraries v2023.2_update1)

The released `kernel_gtsv.cpp` is compiled for a 16x16 system with one compute unit (`#define NRC 16`, `NCU 1`); the
published figure (README_AMD.md: 1024x1024, unroll 16) needs the commented-out `NRC 1024`, `NCU 16`. The host does not
check `-M` against the compiled size: on the 16x16 build, `-M 1024` hangs the kernel (the host waits forever).
Change (innova2): those two defines in `kernel_gtsv.cpp`; the Makefile's paths say `gtsv_1024`. Everything else is AMD's
(Apache-2.0).
