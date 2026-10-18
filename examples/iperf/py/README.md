# iperf: no-compile alternatives

- `bash examples/iperf/py/run_py.sh [xclbin]` is the FPGA iperf2 server, as `../run.sh`;
- `bash examples/iperf/py/run_client_py.sh [xclbin]` is the fns client, as `../run_client.sh`.

Both set up FLD-lite with `fld_lite/py/e5udp_host.py`. Nothing on the data path is Python.

On an HP Z440 (Xeon E5-1603 v4) with r10 the server run gives the same numbers as with the C host:
- 91 295 datagrams/s at 1470 B;
- 0 lost;
- every report accounted for.
