# zuc: no-compile alternative

`bash examples/zuc/py/run_py.sh [xclbin] [requests]` runs the same runs as `../run.sh` with
`fld_lite/py/e5gen_host.py --zuc`. The expected replies come from `fld_lite/py/zuc_model.py`, which gives the same
outputs as the C `zuc_model.h`.

Results on an HP Z440 (Xeon E5-1603 v4), r10:

| run | C host | Python host |
|---|---|---|
| INTEG 8191 B | 19.49 Gb/s | 19.51 Gb/s |
| CONF 4096 B | 9.12 Gb/s | 9.17 Gb/s |

Every run had 0 mismatches and 0 TX errors with both hosts.
