# lat_probe: no-compile alternative

`bash examples/lat_probe/py/run_py.sh [xclbin]` does what `../run.sh` does, with `lat_host.py` (pyxrt) in place of
the C++ `fld_lite/lat/lat_host`. Needs python3, numpy and XRT's pyxrt; nothing is compiled.

On an HP Z440 (Xeon E5-1603 v4) with the r10 shell its output is identical, line for line, to the C host's. The
kernel does the timing, and launch overhead cancels out, so Python's slower dispatch does not show.
