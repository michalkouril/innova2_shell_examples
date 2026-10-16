# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: GPL-2.0-only
obj-m += innova2_hostmem.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
