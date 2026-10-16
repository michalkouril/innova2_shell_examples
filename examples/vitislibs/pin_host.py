#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""pin_host.py <benchmark dir> -- rewrite a Vitis_Libraries benchmark's host sources for the Innova-2: every explicit
memory-bank choice becomes memory index 0 (bank0, the only kernel memory here). The hosts name U200/U250/U50 banks --
XCL_MEM_DDR_BANK1..3, XCL_BANK(n) / XCL_BANKn, 'n | XCL_MEM_TOPOLOGY' -- and on this platform those indices are unused
PLRAM or host memory: XRT refuses them ('failed to allocate userptr bo: Operation not permitted'). Buffers placed by
kernel argument ({arg, ptr, kernel()}) are left alone, as are the macro definitions. Every *.cpp/*.hpp/*.h outside
kernel/ directories; idempotent. Prints the number of places changed.
pin_host.py --check <xclbin> -- exit 1 unless memory index 0 of the xclbin is bank0 (what the rewrite assumes).
pin_host.py --sys-port <benchmark dir> -- the system-compiler benchmarks' SYS_PORT(ptr, DDR[n] / HBM[n]) -> bank0, in
every source (build.sh runs it before the build)."""
import json, os, re, subprocess, sys, tempfile

if sys.argv[1] == '--sys-port':
    # system-compiler (_sc) benchmarks name the memory in the C++: SYS_PORT(ptr, DDR[0]) / SYS_PORT_PFM(u50, ptr, HBM[n])
    # -- no such memory here ("Specified SYS_PORT: DDR[0] ... cannot be found in the platform"); all become bank0
    rx = re.compile(r'(\bSYS_PORT(?:_PFM)?\s*\((?:[^,()]+,\s*)?[^,()]+,\s*)(?:DDR|HBM|PLRAM)\s*\[\s*\d+\s*\](\s*\))')
    n = 0
    for root, dirs, files in os.walk(sys.argv[2]):
        dirs[:] = [x for x in dirs if not x.startswith(('_x', 'build_dir', 'reports', '.'))]
        for fn in files:
            if not fn.endswith(('.cpp', '.hpp', '.h')): continue
            p = os.path.join(root, fn); t = open(p, errors='surrogateescape').read(); t2, k = rx.subn(r'\1bank0\2', t)
            if k: open(p, 'w', errors='surrogateescape').write(t2); n += k
    print(n); sys.exit(0)
if sys.argv[1] == '--check':
    xb = sys.argv[2]; d = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(xb))); f = d + '/m.json'
    try:
        subprocess.run(['xclbinutil', '--input', xb, '--dump-section', 'MEM_TOPOLOGY:JSON:' + f, '--force'],
                       capture_output=True)
        tag = json.load(open(f))['mem_topology']['m_mem_data'][0]['m_tag'] if os.path.exists(f) else None
    finally:
        if os.path.exists(f): os.remove(f)
        os.rmdir(d)
    if tag != 'bank0': print('memory index 0 is %s, not bank0' % tag); sys.exit(1)
    sys.exit(0)

subs = [(re.compile(r'\bXCL_MEM_DDR_BANK[1-9]\b'), 'XCL_MEM_DDR_BANK0'),
        (re.compile(r'\bXCL_BANK\(\s*(?!0\s*\))[^()]*\)'), 'XCL_BANK(0)'),
        (re.compile(r'\bXCL_BANK(?!0\b)[0-9]+\b'), 'XCL_BANK0'),
        (re.compile(r'(\(\s*unsigned\s+int\s*\)\s*)?\(\s*[1-9][0-9]*\s*\)\s*\|\s*XCL_MEM_TOPOLOGY'), '(unsigned int)(0) | XCL_MEM_TOPOLOGY'),
        (re.compile(r'(?<![\w)])[1-9][0-9]*\s*\|\s*XCL_MEM_TOPOLOGY'), '0 | XCL_MEM_TOPOLOGY')]
n = 0
for root, dirs, files in os.walk(sys.argv[1]):
    dirs[:] = [x for x in dirs if x != 'kernel' and not x.startswith(('_x', 'build_dir', 'reports', '.'))]
    for fn in files:
        if not fn.endswith(('.cpp', '.hpp', '.h')): continue
        p = os.path.join(root, fn); out = []; k = 0
        for ln in open(p, errors='surrogateescape'):
            if not re.match(r'\s*#\s*define\b', ln):
                for rx, rep in subs:
                    ln, c = rx.subn(rep, ln); k += c
            out.append(ln)
        if k:
            open(p, 'w', errors='surrogateescape').write(''.join(out)); n += k
print(n)
