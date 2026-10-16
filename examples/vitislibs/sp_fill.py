#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""sp_fill.py <v++ temp dir> <out cfg> [cfg ...] -- write <out cfg> with an 'sp=<cu>.<arg>:bank0' line for every
memory argument of every compute unit that the cfgs do not pin already (by argument or by interface name). Some
benchmarks' cfgs have only nk= / stream_connect= lines (data_compression/L2/demos/lz4_streaming), and an unpinned port
lands in the platform default (PLRAM0 here), which mem_check.py then rejects. Memory arguments: addressQualifier 1 in
each compiled kernel's kernel.xml (<temp>/*.xo). Compute units: nk=<k>:<n>[:<cu>.<cu>...] in the cfgs, else
<k>_1 .. <k>_<n>, else <k>_1.
The out cfg holds only sp= lines (a second cfg with nk= / stream_connect= would define them twice). Prints the count."""
import glob, re, sys, zipfile
import xml.etree.ElementTree as ET
tmp, out, cfgs = sys.argv[1], sys.argv[2], sys.argv[3:]
nk, pinned = {}, set()
for c in cfgs:
    try: lines = open(c, errors='ignore').read().splitlines()
    except OSError: continue
    for ln in lines:
        m = re.match(r'\s*nk\s*=\s*(\w+)\s*:\s*(\d+)\s*(?::\s*(\S+))?', ln)
        if m:
            k, n = m.group(1), int(m.group(2))
            nk[k] = m.group(3).split('.') if m.group(3) else ['%s_%d' % (k, i + 1) for i in range(n)]
        m = re.match(r'\s*sp\s*=\s*([\w]+)\.([\w]+)\s*:', ln)
        if m: pinned.add((m.group(1), m.group(2)))
lines = []
for xo in sorted(glob.glob(tmp + '/*.xo')):
    z = zipfile.ZipFile(xo)
    for n in z.namelist():
        if not n.endswith('/kernel.xml'): continue
        for k in ET.fromstring(z.read(n)).iter('kernel'):
            args = [(a.get('name'), a.get('port', '').lower()) for a in k.iter('arg')
                    if a.get('addressQualifier') == '1']
            for cu in nk.get(k.get('name'), [k.get('name') + '_1']):
                # a cfg pins an argument by its name or by its interface (sp=<cu>.m_axi_gmem0:...)
                lines += ['sp=%s.%s:bank0' % (cu, a) for a, port in args
                          if (cu, a) not in pinned and (cu, port) not in pinned]
open(out, 'w').write('[connectivity]\n' + ''.join(l + '\n' for l in lines))
print(len(lines))
