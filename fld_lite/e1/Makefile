# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
KVER ?= $(shell uname -r)
IFC  ?= /usr/src/ofa_kernel/x86_64/$(KVER)/include/linux/mlx5
XRT  ?= /opt/xilinx/xrt

e1_sq: e1_sq.cpp devx_sq.o
	g++ -std=c++17 -O2 -Wall -o $@ e1_sq.cpp devx_sq.o -I$(XRT)/include -L$(XRT)/lib -lxrt_coreutil -libverbs -lmlx5 \
	    -Wl,-rpath,$(XRT)/lib
devx_sq.o: devx_sq.c devx_sq.h prm.h mlx5_ifc.h
	gcc -std=gnu11 -O2 -Wall -Wno-unused-function -c devx_sq.c
# PRM layouts from the running OFED tree (dual GPL-2.0 / OpenIB BSD) -- copied at build time, not kept in the repo
mlx5_ifc.h:
	cp $(IFC)/mlx5_ifc.h $(IFC)/mlx5_ifc_fpga.h .
clean:
	rm -f e1_sq devx_sq.o mlx5_ifc.h mlx5_ifc_fpga.h
