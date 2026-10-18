# memcached: no-compile alternative

`bash examples/memcached/py/run_py.sh [xclbin]` runs the same flow as `../run.sh` with Python hosts:
- FLD-lite is set up by `fld_lite/py/e5udp_host.py` (see `fld_lite/py/README.md`);
- the load generator is `mcd_load.py` (worker processes, one datagram per system call), in place of the C `mcd_load`.

The C version's `mcd_load` is built with gcc. Still compiled: the `innova2_peermem` kernel module.

Both versions give the same verdicts on an HP Z440 (Xeon E5-1603 v4), r10, 25 GbE DAC:
- mcd_client 10/10 PASS;
- 0 bad values and 0 lost;
- 18 of 65536 preload SETs refused (full buckets);
- E5UDP-HOST OK.

The load points measure the client, not the FPGA, so they are lower with the Python generator:

| point | C `mcd_load` | Python `mcd_load.py` |
|---|---|---|
| 1 in flight | 87 k req/s | 44 k req/s |
| 4 threads x 64 in flight | 1.42 M req/s | 0.40 M req/s |
