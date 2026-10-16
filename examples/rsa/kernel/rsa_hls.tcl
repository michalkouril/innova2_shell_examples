# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# rsa_modexp needs its small per-lane arrays left as RAMs: partitioned into registers, the lane-indexed selects hide
# the dependence distances from HLS and the main loop falls to II 2-8.
config_array_partition -complete_threshold 0
