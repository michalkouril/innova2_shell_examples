# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# UDP architecture step 0 (host only). On x440: RDMA=~/innova2_kit/rdma/root/usr (local rdma-core 39);
# mlx5_ifc.h comes from an OFED kernel tree (copied in, not kept in the repo).
RDMA ?= /usr
CFLAGS = -O2 -Wall -Wno-unused-function -I. -I$(RDMA)/include
LIBS = -L$(RDMA)/lib/x86_64-linux-gnu -libverbs -lmlx5 -Wl,-rpath,$(RDMA)/lib/x86_64-linux-gnu
nicq: nicq.cpp devx_rx_s0.o devx_sq.o
	g++ -std=c++17 $(CFLAGS) -o $@ nicq.cpp devx_rx_s0.o devx_sq.o $(LIBS)
%.o: %.c prm.h mlx5_ifc.h
	gcc -std=gnu11 $(CFLAGS) -c $<
clean:
	rm -f nicq *.o
