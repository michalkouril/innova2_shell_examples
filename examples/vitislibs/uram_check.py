#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
"""uram_check.py <max URAM> <v++ temp dir> [--lut-max N] [cfg ...] -- before the link: the URAM and LUTs the kernels
need, from each compiled kernel's HLS estimate (<temp>/<k>/<k>/<k>/solution/syn/report/<k>_csynth.xml) times its
compute units (nk=<k>:<n> in the cfgs, else 1); exit 1 when a total is above its limit. Prints one line per kernel and
the totals.
URAM, measured: the HLS estimate is at or above what synthesis uses (snappy 48+2 = 48+2; naive_bayes 256 vs 128), and
the platform's own logic takes 48 of the dynamic region's 80 URAM sites, so 32 are left for kernels -- a link that
needs more stops at placement DRC (VPL UTLZ-1) after 20-40 minutes.
LUTs, measured: TreeEngine kernels of 344K / 385K / 423K HLS LUTs (329K after synthesis, of the region's ~440K) all
failed in placement after ~40 minutes (VPL 30-488/30-99: the platform's own cells no longer placed), MCAmerican at
~480K failed placement DRC; build.sh passes --lut-max 300000 (VL_LUT_MAX). Without --lut-max LUTs are not checked."""
import glob, os, re, sys
import xml.etree.ElementTree as ET
a = sys.argv[1:]; mx, tmp = int(a[0]), a[1]; a = a[2:]; lmx = None
if a[:1] == ['--lut-max']: lmx, a = int(a[1]), a[2:]
cus = {}
for c in (c for c in a if os.path.isfile(c)):
    for ln in open(c, errors='ignore'):
        m = re.match(r'\s*nk\s*=\s*([\w]+)\s*:\s*(\d+)', ln)
        if m: cus[m.group(1)] = int(m.group(2))
tot = ltot = 0
for x in sorted(glob.glob(os.path.join(tmp, '*', '*', '*', 'solution', 'syn', 'report', '*_csynth.xml'))):
    k = os.path.basename(x)[:-len('_csynth.xml')]
    if x.split(os.sep)[-6] != k: continue                   # only the top function of each kernel
    r = ET.parse(x).getroot()
    def est(t):
        e = r.find('./AreaEstimates/Resources/' + t)
        return int(e.text) if e is not None and e.text else 0
    u, l = est('URAM'), est('LUT')
    n = cus.get(k, 1); tot += u * n; ltot += l * n
    print('   %-32s URAM %4d  LUT %7d  x %d CU' % (k, u, l, n))
print('   URAM total %d (kernel limit %d)' % (tot, mx))
if lmx is not None: print('   LUT total %d (HLS estimate; kernel limit %d)' % (ltot, lmx))
sys.exit(1 if tot > mx or (lmx is not None and ltot > lmx) else 0)
