<!--
SPDX-FileCopyrightText: 2026 the innova2 contributors
SPDX-License-Identifier: Apache-2.0
-->
# Licence audit — innova2_shell_examples (2026-10-17)

Every file in this repository, classified by provenance (checked by hand; the evidence column quotes the file's own
header, "REUSE.toml" where the licence is given there). NOTICE describes the third-party material; REUSE.toml and the
SPDX headers are the machine-readable licence of every file (`reuse lint`: compliant).

**Provenance checks made for this audit:** the unmodified AMD files in examples/vitislibs/overlay are byte-identical
to Vitis_Libraries v2023.2_update1 (the AES 4-PU kernel and host are v2020.2_rel, listed in its README); changed AMD
files carry change notices (Apache-2.0 §4b). No AMD-generated RTL, IP output (.xci/.dcp) or proprietary source is in
the tree; the only RTL is the BSD-2-Clause ZUC AFU and our wrappers. mlx5_ifc.h (OFED) is copied at build time, not
included. The xclbins hold bitstreams plus metadata (no IP source); their only embedded build path is the platform
location on the build machine. No credentials, e-mail addresses, serial numbers or public IP addresses in the tree.

| file | class | licence evidence in the file |
|---|---|---|
| `.gitignore` | Ours | — (REUSE.toml) |
| `LICENSE` | Licence text | copyright owner or entity authorized by the copyright owner that is granting th; copyright notice that is incl |
| `LICENSES/Apache-2.0.txt` | Licence text | copyright owner or entity authorized by the copyright owner that is granting th; copyright notice that is incl |
| `LICENSES/BSD-2-Clause.txt` | Licence text | Copyright (c) <year> <owner>; copyright notice, this; copyright notice,; COPYRIGHT HOLDERS AND CONTRIBUTORS "A |
| `LICENSES/BSD-3-Clause.txt` | Licence text | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `LICENSES/CC-BY-4.0.txt` | Licence text | licensed under their terms and conditions, or any related inform; copyright and certain other rights specified |
| `LICENSES/GPL-2.0-only.txt` | Licence text | Copyright (C) 1989, 1991 Free Software Foundation, Inc.,; copyright the software, and |
| `LICENSES/LicenseRef-AMD-IP.txt` | Licence text | — (REUSE.toml) |
| `LICENSES/Linux-OpenIB.txt` | Licence text | copyright notice, this; copyright notice,; COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER |
| `LICENSE_AUDIT.md` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0; copyright owner or |
| `NOTICE` | Ours | Copyright 2026 the innova2 contributors. Licensed under the Apache License, Ver; Copyright (c) 2020-2021 Gabi  |
| `README.md` | Ours | — (REUSE.toml) |
| `REUSE.toml` | Ours | CopyrightText = ["2026 the innova2 contributors"]; CopyrightText = ["2026 the innova2 contributors", "2019-202 |
| `build_all.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `common/build_lib.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `common/card_env.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `common/link_gate.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `common/net_lib.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `common/pyxrt_util.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/fetch_vitis_libs.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gunzip/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gunzip/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/gzip/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gzip/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/gzip8/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gzip8/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/gzip_combo/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gzip_combo/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/gzip_figz/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gzip_figz/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/gzip_x4/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/gzip_x4/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/host/build_hosts.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/host/gzip_xn_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/lib.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/lz4/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/lz4/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/lz4x4/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/lz4x4/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/lz4x8/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/compression/lz4x8/conn.cfg` | Ours | — (REUSE.toml) |
| `examples/compression/patches/gzip_bram.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/patches/gzip_hist.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/patches/gzip_host_isize.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/patches/host_mcr.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/patches/host_timing.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/patches/lz4_dict_bram.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/compression/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0; copyright files),  |
| `examples/fld_echo/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/fld_echo/link.cfg` | Ours | — (REUSE.toml) |
| `examples/fld_echo/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/h4ml_client.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/jets.bin` | Dataset derivative, CC-BY-4.0 | binary |
| `examples/hls4ml/kernel/firmware/defines.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/jetnet.cpp` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/jetnet.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_activation.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_activation_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_batchnorm.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_batchnorm_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_code_gen.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_common.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv1d.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv1d_latency.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv1d_resource.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv1d_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv2d.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv2d_latency.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv2d_resource.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv2d_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_conv_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_dense.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_dense_compressed.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_dense_latency.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_dense_resource.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_dense_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_depthwise_product.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_embed.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_embed_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_function_stubs.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_garnet.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_helpers.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_image.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_image_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_math.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_merge.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_merge_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_mult.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_padding.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_padding_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_pooling.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_pooling_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_recr_activations.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_recurrent.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv1d.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv1d_latency.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv1d_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv2d.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv2d_latency.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv2d_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_sepconv_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_transpose.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_transpose_stream.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/nnet_utils/nnet_types.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/parameters.h` | hls4ml (generated / templates), Apache-2.0 | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b2.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b2.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b4.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b4.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b6.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b6.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b8.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/b8.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w2.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w2.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w4.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w4.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w6.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w6.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w8.h` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/firmware/weights/w8.txt` | Ours, generated data | — (REUSE.toml) |
| `examples/hls4ml/kernel/h4ml_jet.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/kernel/h4ml_jet_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/link.cfg` | Ours | — (REUSE.toml) |
| `examples/hls4ml/model/convert.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/model/make_jets.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/model/train.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hls4ml/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hostmem/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/hostmem/link.cfg` | Ours | — (REUSE.toml) |
| `examples/hostmem/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/README.md` | Ours | — (REUSE.toml) |
| `examples/iperf/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/build_client.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/fetch_fns.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/iperf_sctl.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/kernels/iperf_fns.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/kernels/iperf_fns_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/kernels/iperf_kernel.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/kernels/iperf_kernel_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/link.cfg` | Ours | — (REUSE.toml) |
| `examples/iperf/link_client.cfg` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/iperf/run_client.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/lat_probe/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/lat_probe/link.cfg` | Ours | — (REUSE.toml) |
| `examples/lat_probe/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/kernels/mcd_hls.tcl` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/kernels/mcd_kernel.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/kernels/mcd_kernel_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/kernels/mcd_load_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/kernels/pipeline/globals.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/globals.h` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/cc.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/compare.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/hash.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/hashTable.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/hashTableWithProfiler.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/memRead.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/memWrite.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/hashTable/memWriteWithProfiler.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/kvsPipelineHostMem.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/memcachedPipeline.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/memcachedPipeline_tb.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/merger.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/dramModel/dramModel.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/dramModel/dramModel.h` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/dummyPCIeAddressAllocation/dummyPCIeJoint.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/dummyPCIeAddressAllocation/globals.h` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/flashModel/flashModel.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/otherModules/flashModel/flashModel.h` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/requestParser/requestParser.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/responseFormatter/asciiResponse.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/responseFormatter/binResponse.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/responseFormatter/responseFormatter.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/splitter.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/statsCollectorIn.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/valueStore/flashValueStore.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/kernels/pipeline/valueStore/valueStore.cpp` | Xilinx HLx memcached, BSD-3-Clause | Copyright (c) 2016, Xilinx, Inc.; copyright notice,; copyright holder nor the names of its contributors; COPYR |
| `examples/memcached/link.cfg` | Ours | — (REUSE.toml) |
| `examples/memcached/mcd_client.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/mcd_load.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/memcached/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/DESIGN.md` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/README.md` | Ours | — (REUSE.toml) |
| `examples/ntp/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/kernels/ntp_kernel.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/kernels/ntp_kernel_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/link.cfg` | Ours | — (REUSE.toml) |
| `examples/ntp/ntp_client.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/ntp_lib.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/ntp_load.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/ntp_servo.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/ntp/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/kernel/make_vectors.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/kernel/rsa_hls.tcl` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/kernel/rsa_modexp.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/kernel/rsa_modexp_tb.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/link.cfg` | Ours | — (REUSE.toml) |
| `examples/rsa/rsa_host.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/rsa/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/build_hosts.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/examples.txt` | Ours | — (REUSE.toml) |
| `examples/suite/fetch_vitis_accel_examples.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/gen_cfg.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/mem_check.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/prep_cfg.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/py/README.md` | Ours | — (REUSE.toml) |
| `examples/suite/py/hello_world.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_asynchronous_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_copy_buffer_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_data_transfer_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_kernel_chain.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_mailbox_auto_restart_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_mult_compute_units_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_streaming_free_running_k2k_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/host_xrt_streaming_k2k_mm_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/performance_axi_burst_performance.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/performance_iops_test_xrt.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_adder_streams.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_streaming_free_running_k2k.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_streaming_k2k_mm.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_vadd_2clks.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_vadd_2kernels.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_vadd_hw_debug.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/rtl_kernels_rtl_vadd_mixed_c_vadd.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/run_py.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/py/suite_lib.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/py/sys_opt_kernel_swap.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/py/sys_opt_multiple_process.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: 2019-2021 Xilinx, Inc.; SPDX-Li |
| `examples/suite/rtl_ports.map` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/suite/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/benchmarks.txt` | Ours | — (REUSE.toml) |
| `examples/vitislibs/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/build_hosts.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/check_resize.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/clean_temp.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/fetch.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/graphdata/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/graphdata/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/graphdata/prepare.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/graphdata/tools/csr_text.h` | Ours | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/lp_csim.cpp` | Ours + AMD Apache-2.0 code | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/mtx2csr.c` | Ours | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/sssp_csim.cpp` | Ours + AMD Apache-2.0 code | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/sssp_ref.c` | Ours | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/tc_csim.cpp` | Ours + AMD Apache-2.0 code | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/tc_ref.c` | Ours | — (REUSE.toml) |
| `examples/vitislibs/graphdata/tools/twohop_ref.c` | Ours | — (REUSE.toml) |
| `examples/vitislibs/host_patches/graph/L2/benchmarks/label_propagation.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/vitislibs/host_patches/graph/L2/benchmarks/shortest_path_float_pred.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/vitislibs/host_patches/graph/L2/benchmarks/triangle_count.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/vitislibs/host_patches/graph/L2/benchmarks/twoHop.patch` | Ours, patches to AMD Apache-2.0 files | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/Makefile` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/README_AMD.md` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/conn_u200.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/conn_u50_u280.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/description.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/host/test_decoder.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/images/t0.jpg` | AMD Vitis Libraries, Apache-2.0 | binary |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/images/t1.jpg` | AMD Vitis Libraries, Apache-2.0 | binary |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/images/t2.jpg` | AMD Vitis Libraries, Apache-2.0 | binary |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/kernel/XAcc_idct.hpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/kernel/kernelJpegDecoder.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/kernel/kernelJpegDecoder.hpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/codec/L2/demos/jpegDec_fix/utils.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/Makefile` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/README_AMD.rst` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/config.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/connectivity.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/data/sample.txt` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "License |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/description.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/kernel/snappy_decompress_details.hpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "Li |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/qor.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/run.sh` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/sample.txt` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "License |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/src/host.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "Li |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/utils.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/data_compression/L2/demos/snappy_streaming_fix/xrt.ini` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/Makefile` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/README_AMD.rst` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/connectivity.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/data/sample.txt` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "License |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/data/sample.txt.zst` | AMD Vitis Libraries, Apache-2.0 | binary |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/description.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/kernel/zstd_decompress.hpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "Li |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/kernel/zstd_decompress_stream.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "Li |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/run.sh` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/sample.txt` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "License |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/sample.txt.zst` | AMD Vitis Libraries, Apache-2.0 | binary |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/src/host.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019-2022 Xilinx, Inc. All rights reserved.; Licensed under the Apache License, Version 2.0 (the "Li |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/utils.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/data_compression/L2/demos/zstd_decompress_fix/xrt.ini` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/Makefile` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/README.md` | Ours | Copyright Xilinx, Inc.): |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/conn_u250.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/conn_u50.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/description.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/host/main.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/kernel/aes256CbcDecryptKernel.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/utils.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/security/L1/benchmarks/aes256CbcDecrypt_4pu/xrt.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/Makefile` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/README.md` | Ours | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/README_AMD.md` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/conn_u200.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/conn_u250.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/conn_u280.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/conn_u50.cfg` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/description.json` | AMD Vitis Libraries, Apache-2.0 | — (REUSE.toml) |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/kernel_gtsv.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/test_gtsv.cpp` | AMD Vitis Libraries, Apache-2.0 | Copyright 2019 Xilinx, Inc.; Licensed under the Apache License, Version 2.0 (the "License"); |
| `examples/vitislibs/overlay/solver/L2/benchmarks/gtsv_1024/utils.mk` | AMD Vitis Libraries, Apache-2.0 | Copyright (C) 2019-2022, Xilinx, Inc.; Copyright (C) 2022-2023, Advanced Micro Devices, Inc.; Licensed under t |
| `examples/vitislibs/pin_cfg.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/pin_host.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/py/Quadrature.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/README.md` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/py/SVD.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/adler32.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/aes256CbcDecrypt_4pu.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/compound_sort.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/crc32.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/gesvdj.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/graph_common.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/gtsv.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/jpegDec_fix.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/label_propagation.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/lz4_streaming.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/maximal_independent_set.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/resize.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/run_py.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/py/shortest_path_float_pred.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/snappy_streaming_fix.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/triangle_count.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/twoHop.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/vl_stream.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/py/zstd_decompress_fix.py` | Ours, Python port of an AMD host, Apache-2.0 | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Advanced Micro Devices, Inc.; S |
| `examples/vitislibs/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/sp_fill.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/vitislibs/uram_check.py` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/zuc/build.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `examples/zuc/link.cfg` | Ours | — (REUSE.toml) |
| `examples/zuc/run.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/README.md` | Ours | — (REUSE.toml) |
| `fld_lite/devx/devx_cq.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_cq.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_rx.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_rx.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_rx_s0.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_rx_s0.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_sq.c` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/devx_sq.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/devx/prm.h` | Ours, with Mellanox-derived macros | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-FileCopyrightText: Mellanox Technologies Ltd.; SPD |
| `fld_lite/e5/Makefile.host` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/e5_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/e5gen_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/e5udp_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/fld_lite.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/fld_lite_hls.tcl` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/gen/gen_sim.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/gen/gen_sink.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/sim/burst_maxi_mt.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/sim/fld_sim.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/udp_lite.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/udp_swap.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/udp_swap_tb.cpp` | Ours | — (REUSE.toml) |
| `fld_lite/e5/zuc/LICENSE.flexdriver-zuc` | Licence text | Copyright (c) 2020-2021 Gabi Malka; copyright notice, this; copyright notice,; COPYRIGHT HOLDERS AND CONTRIBUT |
| `fld_lite/e5/zuc/adapt_test.cpp` | Ours | — (REUSE.toml) |
| `fld_lite/e5/zuc/package_zuc_k.tcl` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/zuc/rtl4/zuc_afu.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/rtl4/zuc_core.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/rtl4/zuc_histo.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/rtl4/zuc_ip_synth.v` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: BSD-2-Clause |
| `fld_lite/e5/zuc/rtl4/zuc_k.v` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: BSD-2-Clause |
| `fld_lite/e5/zuc/rtl4/zuc_module.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/rtl4/zuc_module_wrapper.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/rtl4/zuc_params.v` | FlexDriver ZUC AFU, BSD-2-Clause | Copyright (c) 2021 Gabi Malka.; Licensed under the 2-clause BSD license, see LICENSE for details; SPDX-License |
| `fld_lite/e5/zuc/sim_rtl/gen_check.py` | Ours | — (REUSE.toml) |
| `fld_lite/e5/zuc/sim_rtl/tb_k250.v` | Ours | — (REUSE.toml) |
| `fld_lite/e5/zuc/sim_rtl/zuc_ref.py` | FlexDriver ZUC AFU, BSD-2-Clause | — (REUSE.toml) |
| `fld_lite/e5/zuc/zuc_adapt.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/zuc/zuc_model.h` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/e5/zuc/zuc_sbox.h` | FlexDriver ZUC AFU, BSD-2-Clause | — (REUSE.toml) |
| `fld_lite/e5/zuc/zuc_selftest.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/host_setup.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/hostmem/Makefile` | Ours, GPL-2.0-only | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: GPL-2.0-only |
| `fld_lite/hostmem/hostmem_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/hostmem/hostmem_probe.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/hostmem/innova2_host_retag.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/hostmem/innova2_hostmem.c` | Ours, GPL-2.0-only | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: GPL-2.0-only |
| `fld_lite/hostmem/link.cfg` | Ours | — (REUSE.toml) |
| `fld_lite/innova2_peermem/Makefile` | Ours, GPL-2.0-only | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: GPL-2.0-only |
| `fld_lite/innova2_peermem/innova2_peermem.c` | Ours, GPL-2.0-only | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: GPL-2.0-only |
| `fld_lite/lat/lat_capture.tcl` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/lat/lat_host.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/lat/lat_probe.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `fld_lite/lat/lat_probe_l1.cpp` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `tools/figz/README.md` | Ours | — (REUSE.toml) |
| `tools/figz/figz` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0 |
| `tools/figz/test_figz.sh` | Ours | SPDX-FileCopyrightText: 2026 the innova2 contributors; SPDX-License-Identifier: Apache-2.0; copyright files),  |
| `xclbin/MANIFEST.md` | Ours, generated data | — (REUSE.toml) |
| `xclbin/SHA256SUMS` | Ours, generated data | — (REUSE.toml) |
| `xclbin/fld_echo.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gunzip.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gzip.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gzip8.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gzip_combo.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gzip_figz.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/gzip_x4.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/hls4ml.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/hostmem.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/iperf.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/iperf_client.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/lat_probe.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/lz4.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/lz4x4.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/lz4x8.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/memcached.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/ntp.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/rsa.xclbin` | Vitis-built binary containing AMD IP | binary |
| `xclbin/zuc.xclbin` | Vitis-built binary containing AMD IP | binary |

## Classes

* **Licence text** (9 files): Verbatim licence texts (SPDX licence list) and our LicenseRef explanation.
* **Ours** (178 files): Written in this project: Apache-2.0, (c) 2026 the innova2 contributors (SPDX header or REUSE.toml).
* **Ours, GPL-2.0-only** (4 files): Our Linux kernel modules (SPDX tag in the file).
* **Ours, generated data** (18 files): Produced by our scripts from our own model or measurements (weights, checksums, manifests).
* **Ours, with Mellanox-derived macros** (1 files): prm.h: accessor macros following Mellanox include/linux/mlx5/device.h (dual GPL-2.0 / OpenIB BSD), used under Linux-OpenIB.
* **FlexDriver ZUC AFU, BSD-2-Clause** (8 files): Gabi Malka's flexdriver-zuc RTL (as published or wrapped by us); keep the BSD-2 notice.
* **Xilinx HLx memcached, BSD-3-Clause** (28 files): Xilinx 2016 memcached pipeline (HLx_Examples 249406bf) with our changes marked "innova2:".
* **AMD Vitis Libraries, Apache-2.0** (58 files): Copies of AMD benchmark directories (unmodified, or changed and marked so); keep the Apache headers.
* **Ours, patches to AMD Apache-2.0 files** (10 files): Our changes to Vitis Libraries files; context lines are AMD's (Apache-2.0).
* **Ours, Python port of an AMD host, Apache-2.0** (40 files): pyxrt host translated from AMD's C++ host (Vitis Libraries / Vitis_Accel_Examples host files: Apache-2.0, Xilinx / AMD); both copyrights in the header.
* **Ours + AMD Apache-2.0 code** (3 files): Our tool that contains or includes AMD Vitis Libraries kernel code (Apache-2.0).
* **hls4ml (generated / templates), Apache-2.0** (53 files): Output of hls4ml from our model; nnet_utils are hls4ml's templates.
* **Dataset derivative, CC-BY-4.0** (1 files): Derived from the hls4ml_lhc_jets_hlf dataset (attribution in NOTICE).
* **Vitis-built binary containing AMD IP** (19 files): xclbin: our kernels (and any third-party kernels listed in NOTICE) plus the AMD IP of the dynamic region, under AMD's terms (LicenseRef-AMD-IP): distributable as a bitstream, to program AMD devices only.
