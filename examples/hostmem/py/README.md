# hostmem: no-compile host

`bash examples/hostmem/py/run_py.sh [xclbin]` runs `hostmem_host.py` (pyxrt) in place of the C++ `hostmem_host`:
the same four tests and the same RESULT line. The kernel module `innova2_hostmem.ko` is still needed. It is a kernel
module, so `run_py.sh` builds it with make (this needs the kernel headers) unless it is already loaded.

On an HP Z440 (Xeon E5-1603 v4) with r10 both hosts PASS. Python: write 6.89 GB/s, read 6.89 GB/s, dependent read
1108 ns. C: 6.99 GB/s, 7.02 GB/s, 1046 ns. The small gap is Python's dispatch inside the timed window.
