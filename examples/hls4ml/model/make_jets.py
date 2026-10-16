#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""make_jets.py <vectors.npz> <jets.bin> -- the client's test file from convert.py's vectors: per jet 16 x int16 inputs
(raw ap_fixed<16,6>), 5 x int16 expected outputs (hls4ml's bit-accurate emulation) and the true label (uint8)."""
import struct, sys
import numpy as np
v = np.load(sys.argv[1]); rec = struct.Struct('<16h5hB')
with open(sys.argv[2], 'wb') as f:
    for x, y, lab in zip(v['in_raw'], v['out_raw'], v['label']):
        f.write(rec.pack(*[int(a) for a in x], *[int(a) for a in y], int(lab)))
print('%d jets -> %s' % (len(v['label']), sys.argv[2]))
