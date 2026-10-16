# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build against the running kernel + MLNX/DOCA-OFED's ib_core (for ib_register_peer_memory_client).
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
# /usr/src/ofa_kernel/default can point at another kernel's tree (it was 5.15 on hpy440) -- use the per-kernel one
OFA  ?= /usr/src/ofa_kernel/x86_64/$(KVER)

obj-m := innova2_peermem.o
# OFED's headers must come FIRST, as for OFED's own modules: ib_core was built against them, and modversions CRCs
# are computed from the argument types (struct peer_memory_client -> sg_table/device) as each side's headers see them.
LINUXINCLUDE := -include $(OFA)/include/linux/compat-2.6.h -I$(OFA)/include -I$(OFA)/include/uapi $(LINUXINCLUDE)
KBUILD_EXTRA_SYMBOLS := $(OFA)/Module.symvers

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KBUILD_EXTRA_SYMBOLS=$(KBUILD_EXTRA_SYMBOLS) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
