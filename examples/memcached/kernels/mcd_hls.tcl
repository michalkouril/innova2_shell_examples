# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# mcd_hls.tcl -- HLS pre-tcl for the memcached kernels. mcd's dataflow processes are free-running (ap_start = 1, see
# mcd_kernel.cpp v4), so they also run while ap_rst is high; with the default reset (control registers only) their
# static state -- the hash table's "memory initialised" flag, the slot allocator's fresh counter, every stage's
# state machine -- advanced during reset against FIFOs held in reset and came out wrong (2026-10-17: table never
# cleared, all 32768 slots handed out into a reset FIFO, every SET failed). 'state' resets those registers too.
config_rtl -reset state
