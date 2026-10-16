#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
#
# innova2_host_retag.sh <in.xclbin> <out.xclbin> -- rename the platform's "host" memory bank to "HOST[0]" in the
# xclbin's MEM_TOPOLOGY and GROUP_TOPOLOGY. v++ only accepts the platform tag as written (sp=...:host; "HOST[0]"/"host[0]" are rejected,
# CFGEN 83-2287), but xocl recognises the host-memory bank only by name (strncasecmp(tag, "HOST[0]", 7)).
set -eu
IN=$1; OUT=$2; W=$(mktemp -d -p "$(dirname "$(readlink -f "$OUT")")" .retag.XXXX); trap 'rm -rf "$W"' EXIT
# xocl reads bank names from GROUP_TOPOLOGY when the xclbin has one (v++ always writes it), so rename in every
# topology section present.
cp "$IN" "$W/cur.xclbin"
for SEC in MEM_TOPOLOGY GROUP_TOPOLOGY; do
  xclbinutil --input "$W/cur.xclbin" --dump-section $SEC:JSON:"$W/s.json" --force >/dev/null 2>&1 || continue
  python3 - "$W/s.json" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); n=0
for top in d.values():
    for m in top.get("m_mem_data", []):
        if m["m_tag"].lower()=="host": m["m_tag"]="HOST[0]"; n+=1
if n!=1: sys.exit("expected exactly one 'host' bank, found %d" % n)
json.dump(d,open(p,"w"),indent=1)
PY
  xclbinutil --input "$W/cur.xclbin" --replace-section $SEC:JSON:"$W/s.json" --output "$W/next.xclbin" --force >/dev/null
  mv "$W/next.xclbin" "$W/cur.xclbin"; echo "  $SEC: host -> HOST[0]"
done
cp "$W/cur.xclbin" "$OUT"
xclbinutil --info --input "$OUT" | grep -A3 "Name: *HOST\[0\]" | head -4
