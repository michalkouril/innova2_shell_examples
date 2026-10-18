# ntp: no-compile alternative

`bash examples/ntp/py/run_py.sh [xclbin]` runs the same flow as `../run.sh` with Python hosts:
- FLD-lite is set up by `fld_lite/py/e5udp_host.py`;
- the load generator is `ntp_load.py`, in place of the C `ntp_load`.

The servo, the client and chronyd are unchanged.

Both versions give the same verdicts on an HP Z440 (Xeon E5-1603 v4), r10:
- NTP-CLIENT PASS;
- every frame accounted for (the six frames dropped on purpose);
- 0 lost and 0 bad in every load point.

The load rates are the client's:

| point | C `ntp_load` | Python `ntp_load.py` |
|---|---|---|
| 1 in flight | 84 k replies/s | 54 k replies/s |
| 4 x 32 in flight | 1.40 M replies/s | 0.52 M replies/s |
