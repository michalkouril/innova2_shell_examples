#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""mem_check.py <xclbin> <mem[,mem...]> -- exit 1 (printing the offending tags) when a kernel memory port is connected
to a memory other than the wanted ones: an unpinned port silently takes the platform default. Streams (dc_*) are not
memories."""
import json, os, subprocess, sys, tempfile
xb, want = sys.argv[1], set(sys.argv[2].split(','))
d = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(xb)))
# an xclbin whose kernels have no memory ports has no CONNECTIVITY section (xclbinutil then fails the dump): nothing
# to check
try:
    for sec, f in (('MEM_TOPOLOGY', 'm'), ('CONNECTIVITY', 'c')):
        subprocess.run(['xclbinutil', '--input', xb, '--dump-section', '%s:JSON:%s/%s.json' % (sec, d, f), '--force'],
                       capture_output=True)
    m = json.load(open(d + '/m.json'))['mem_topology']['m_mem_data'] if os.path.exists(d + '/m.json') else []
    c = json.load(open(d + '/c.json'))['connectivity']['m_connection'] if os.path.exists(d + '/c.json') else []
finally:
    for f in ('c.json', 'm.json'):
        if os.path.exists(os.path.join(d, f)): os.remove(os.path.join(d, f))
    os.rmdir(d)
bad = sorted({m[int(x['mem_data_index'])]['m_tag'] for x in c} - want)
bad = [t for t in bad if not t.startswith('dc_')]
print(' '.join(bad)); sys.exit(1 if bad else 0)
