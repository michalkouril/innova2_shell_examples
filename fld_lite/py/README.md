# FLD-lite hosts in Python (no compiler)

`e5udp_host.py`, `e5gen_host.py` and `e5_host.py` do what `../e5/e5udp_host`, `e5gen_host` and `e5_host` do —
same options, same ConnectX queues, same output lines and verdicts — but need no compiler: they run on
`python3` + `numpy` + XRT's `pyxrt`, and reach the ConnectX through `ctypes` on the installed `libibverbs.so.1` /
`libmlx5.so.1` (DevX). The examples' `py/run_py.sh` scripts use them (through `common/net_lib_py.sh`).

| file | what it is |
|---|---|
| `ifc.py` | reads OFED's `mlx5_ifc.h` at run time and turns its `u8 field[width]` layouts into offsets, as the C compiler does for `../devx/prm.h` — no PRM offset is copied by hand |
| `verbs.py` | the libibverbs / libmlx5 calls (`ctypes`), with the rdma-core ABI structures |
| `devx.py` | `../devx/devx_{cq,sq,rx,rx_s0}.c` in Python |
| `fld.py` | the set-up the three hosts share (card, FPGA-memory views, peer_window, the FLD-lite port's queues) |
| `zuc_model.py` | `../e5/zuc/zuc_model.h` in Python (expected replies for `e5gen_host.py --zuc`) |
| `devx_check.py` | host-only check (no FPGA, no traffic): creates every DevX object the hosts create, queries each back |

Checks done on an HP Z440 (Xeon E5-1603 v4) with the r10 shell: `devx.py` builds every DevX command byte for byte
as the C helpers do (both dumped through stub entry points and compared), `ifc.py`'s offsets equal the C
compiler's, and `zuc_model.py` gives the C model's ciphertext and MACs.

Differences from the C hosts, none of them visible to the FPGA kernels:

* pyxrt passes 32-bit scalar kernel arguments, and `fld_tx`'s `buf_va` is 64-bit; the data MR is therefore
  registered with an I/O virtual address equal to the buffer offset (`ibv_reg_mr_iova`), so `buf_va` fits in 32
  bits. The host reads the argument back from the CU's registers and reports a mismatch (see `fld.py`).
* `e5_host.py` sends from port 1 through a Linux `AF_PACKET` socket rather than a verbs raw-packet QP: its round
  trip includes the kernel's network stack and there are no NIC timestamps for port 1, so `--lat` reports the round
  trip and the FPGA's own echo time (port 0 RX CQE -> TX CQE, exact) but not the wire times.
* `--plram` (the fp7 layout) is ported but was not run: the r10 shell has no p2p_alias.

Still compiled: the `innova2_peermem` kernel module (it lets the ConnectX register FPGA memory). The run scripts load
it if it is not loaded; build it once with `make -C fld_lite/innova2_peermem`.
