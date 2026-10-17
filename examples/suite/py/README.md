# Python (pyxrt) hosts for the suite

One Python host per example of `../examples.txt`, a port of the host AMD ships with that example
(Vitis_Accel_Examples commit d1a35c39, `<example>/src/host.cpp`): the same sizes, data patterns and checks, through
XRT's Python binding instead of C++/OpenCL. With the prebuilt xclbins a card's host needs only `python3`, `numpy` and
XRT (`source /opt/xilinx/xrt/setup.sh` puts `pyxrt` on the path) -- no Vitis_Accel_Examples checkout, no compiler.

```
bash examples/suite/py/run_py.sh <root> [example ...]     # <root>/<example>/build_dir.hw.innova2_innova2_xdma_202320_4/*.xclbin
python3 examples/suite/py/hello_world.py <xclbin | build_dir>  # one example; ends with PYHOST-PASS / PYHOST-FAIL
```

`<root>` is the tree `examples/suite/build.sh` leaves (`$BUILD/suite`) or the prebuilt xclbins unpacked. The device
is XRT's device 0, or `INNOVA2_DEV` (index or BDF). `../../../common/pyxrt_util.py` holds the shared layer (device,
xclbin, buffers in the kernel argument's bank, timing); `suite_lib.py` the ports' common bits.

| example | what the port checks (as AMD's host) | where it differs |
|---|---|---|
| hello_world | vadd, 4096 ints | — |
| host_xrt/asynchronous_xrt | vadd of 1M ints; the two input syncs run concurrently, the run waits for both, the output sync follows | pyxrt has no `xrt::queue`: the same ordering from a Python thread pool |
| host_xrt/copy_buffer_xrt | a = 13, b = copy of a, vector_add(a, b) = 26 | pyxrt has no `bo.copy`: copied through the host (after reading a back from the device) |
| host_xrt/data_transfer_xrt | `bo.write` / `bo.read` (no mapping), dummy_kernel copies 256 ints | — |
| host_xrt/kernel_chain | 3 chained 32x32 int matrix products, 10 runs in flight, ap_ctrl_chain vs ap_ctrl_hs kernel, both checked, times printed | random inputs from numpy, not glibc `rand()` |
| host_xrt/mailbox_auto_restart_xrt | auto-restart run (`pyxrt.autostart(0)`), 20 mailbox writes / reads (`pyxrt.mailbox`), then stop | — |
| host_xrt/mult_compute_units_xrt | 4 runs in flight on one kernel object, spread by XRT over the 4 CUs | — |
| host_xrt/streaming_free_running_k2k_xrt | mem_read -> free-running increment -> mem_write, 1M ints | — |
| host_xrt/streaming_k2k_mm_xrt | stream vadd -> stream vmult, 4K ints, (a + b) * c | — |
| performance/axi_burst_performance | 6 kernels x 2 widths x write / read of 16 MB, throughput from the kernels' cycle counts at `-f` MHz (default 300, as AMD's) | every kernel's error count must be 0 (AMD's host keeps only the last) |
| performance/iops_test_xrt | 10000 prepared runs of `hello`, command counts 10 .. 1M, commands/s | the loop is Python (same rate, see below); also checks one buffer holds "Hello World" |
| rtl_kernels/rtl_adder_streams | input stage -> adder (+10) -> output stage, 4096 ints | — |
| rtl_kernels/rtl_streaming_free_running_k2k | mm2s -> free-running RTL adder (+1) -> s2mm, 16M ints | numpy random inputs |
| rtl_kernels/rtl_streaming_k2k_mm | mm2s -> myadder1 -> myadder2 (host-started, no arguments) -> s2mm, 1M ints, +2 | numpy random inputs |
| rtl_kernels/rtl_vadd_2clks | two-clock RTL vadd, 256 ints | — |
| rtl_kernels/rtl_vadd_2kernels | RTL vadd -> copy -> RTL vadd, 256 ints | no `bo.copy` (host copy, as copy_buffer_xrt) |
| rtl_kernels/rtl_vadd_mixed_c_vadd | RTL vadd -> copy -> HLS vadd, 256 ints | no `bo.copy` (host copy) |
| rtl_kernels/rtl_vadd_hw_debug | RTL vadd with an ILA, 256 ints | the ILA is read with Vivado's hardware manager, not by any host |
| sys_opt/kernel_swap | krnl_vmul.xclbin, then (context released) krnl_vadd.xclbin on the same card | — |
| sys_opt/multiple_process | 3 processes (vadd / vsub / vmul) share one xclbin and the card | Python `multiprocessing` ('spawn') instead of `fork()` |

Like AMD's hosts the ports compare whole result arrays; where AMD's `memcmp` compares only `DATA_SIZE` bytes of an
int array (hello_world, the host_xrt examples), the port is stricter.

## Results (hpy440, inn2x r10, XRT 2.27, 2026-10-17)

All 20 pass, and so do AMD's C++ hosts on the same card the same day (`../run.sh`: 20/20 TEST PASSED; no DMA errors
either way). Each port takes about one second including the xclbin load (iops_test 8 s).

* axi_burst_performance: identical numbers -- READ at -f 300, burst 32 / 32 outstanding: 8.94 GB/s (256 bit),
  14.12 GB/s (512 bit); burst 4 / 4: 1.41 / 2.81 GB/s. The WRITE runs report 0 cycles on this card with both hosts
  (AMD's prints `inf GB/sec`, the port too). The figures assume the kernels run at -f MHz.
* iops_test_xrt: the Python loop reaches the C++ host's rate -- 253k-267k commands/s at 1M commands vs 250k (both
  ~60-90k at 10 commands): the card's command path (ERT), not the host language, sets it.
* kernel_chain (10 runs of three 32 x 32 products): Python 2.46 ms chained / 2.38 ms handshake; AMD's OpenCL host
  14.4 / 6.7 ms (its OpenCL buffers migrate host memory each run). At this size dispatch, not the kernel, dominates.
