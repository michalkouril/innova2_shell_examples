#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# hostmem: an RP kernel (hostmem_probe) reads and writes HOST RAM through the shell's peer_window windows 2/3, pointed
# at DMA-coherent host buffers from innova2_hostmem.ko.
. "$(dirname "$0")/../../common/build_lib.sh"
begin hostmem
xo hostmem_probe $REPO/fld_lite/hostmem/hostmem_probe.cpp
link hostmem $REPO/examples/hostmem/link.cfg
