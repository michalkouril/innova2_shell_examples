# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# E2 receive kernel for the inn2x r3 platform. Build (on the Vitis host, ~20-40 min link):
#   source /opt/Xilinx/Vitis/2023.2/settings64.sh; make e2_rx.xclbin
PATCH_XCLBIN ?= patch_xclbin.sh
SCRATCH_XSABIN ?= scratch.xsabin
PLATFORM ?= innova2_innova2_xdma_202320_4
JOBS     ?= 2

e2_rx.xo: e2_rx.cpp
	v++ -c -t hw --platform $(PLATFORM) -k e2_rx -o $@ $<

connectivity.cfg:
	printf '[connectivity]\nsp=e2_rx_1.mem:bank0\nsp=e2_rx_1.data:bank0\nsp=e2_rx_1.res:bank0\n' > $@

e2_rx.xclbin: e2_rx.xo connectivity.cfg
	v++ -l -t hw --platform $(PLATFORM) --config connectivity.cfg --vivado.synth.jobs $(JOBS) --vivado.impl.jobs $(JOBS) -o $@ e2_rx.xo

clean:
	rm -rf e2_rx.xo e2_rx.xclbin* connectivity.cfg _x .Xil *.log v++_*.log .ipcache

# v3 (throughput): dataflow kernel, five ports on bank0; patched for the inn2x ROM timestamp after the link
e2_rx3.xo: e2_rx3.cpp
	v++ -c -t hw --platform $(PLATFORM) -k e2_rx3 -o $@ $<
connectivity3.cfg:
	printf '[connectivity]\nsp=e2_rx3_1.cqp:bank0\nsp=e2_rx3_1.ctl:bank0\nsp=e2_rx3_1.data:bank0\nsp=e2_rx3_1.res:bank0\nsp=e2_rx3_1.dbw:bank0\n' > $@
e2_rx3.xclbin: e2_rx3.xo connectivity3.cfg
	v++ -l -t hw --platform $(PLATFORM) --config connectivity3.cfg --vivado.synth.jobs $(JOBS) --vivado.impl.jobs $(JOBS) -o $@ e2_rx3.xo
e2_rx3_p.xclbin: e2_rx3.xclbin
	bash $(PATCH_XCLBIN) $(CURDIR)/$< $(CURDIR)/$@ $(SCRATCH_XSABIN)
