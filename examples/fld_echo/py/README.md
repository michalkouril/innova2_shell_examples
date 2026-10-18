# fld_echo: no-compile alternative

`bash examples/fld_echo/py/run_py.sh [xclbin] [frames]` runs the same sweep as `../run.sh`, using
`fld_lite/py/e5gen_host.py` and `fld_lite/py/e5_host.py`.

**Sweep.** The FPGA generates and checks the traffic, so the rates and verdicts match the C host's. On an HP Z440
(Xeon E5-1603 v4) with r10, 64 B to 1500 B, both hosts showed 0 mismatches and nearly identical frame counts.

**Idle latency.** `e5_host.py` sends from port 1 through a Linux AF_PACKET socket instead of a verbs queue. Its round
trip therefore includes the kernel's network stack. There are no NIC timestamps for the wire legs.

| measurement | C host | Python host |
|---|---|---|
| round trip, p50 | 4.0 us | 28 us |
| FPGA echo (port 0 RX CQE -> TX CQE), p50 | 1.68 us | 1.69 us |

The Python host also prints RESULT PASS.
