#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""pin_cfg.py <cfg> -- rewrite a Vitis_Libraries connectivity cfg for the Innova-2: every sp= memory (HBM[n], DDR[n],
PLRAM, bank<n>) becomes bank0, the only kernel memory here (an unpinned port would land in host memory); slr= lines go
(one SLR). Everything else is kept. Prints the number of ports pinned."""
import re, sys
p = sys.argv[1]; out = []; n = 0
for ln in open(p, errors='ignore'):
    s = ln.rstrip('\n')
    m = re.match(r'(\s*sp\s*=\s*[\w.]+?\.[\w]+\s*:\s*)(\S+)', s)
    if m:
        out.append(m.group(1) + 'bank0'); n += 1
    elif re.match(r'\s*slr\s*=', s):
        continue
    else:
        out.append(s)
open(p, 'w').write('\n'.join(out) + '\n'); print(n)
