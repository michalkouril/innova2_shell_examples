# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# HLS setting every FLD-lite compile needs (v++ -c ... --hls.pre_tcl fld_lite_hls.tcl): let Vivado choose the m_axi
# data-buffer memory (the default "block" puts even 2-beat 514-bit buffers in BRAM). Measured 2026-10-03 on an earlier shell
# (E5.5 A/B/A): BRAM 45.5 -> 8.5 BRAM36, idle echo 2.17 -> 1.94 us at 64 B, +10% frames/s at 192-512 B.
config_interface -m_axi_buffer_impl auto
