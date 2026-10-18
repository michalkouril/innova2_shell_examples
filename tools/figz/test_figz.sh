#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# test_figz.sh [work dir] -- figz (gzip on the card) against stock gzip; every result compared with the original.
#
# What it shows: figz compresses on the FPGA and writes standard gzip files: gzip -t / gzip -d accept them, figz -d
#   restores them, and figz -d reads stock gzip output. Also: multi-member files, --chunk, -t, the refusal to overwrite,
#   the --cpu path, short last blocks, a non-gzip input, and the resident helper (a background process that keeps the
#   card open, so short runs skip XRT's start-up).
#   Inputs, made once in the work dir: text (the system's /usr/share/doc/*/copyright files, repeated), random and
#   zeros (32 MB each), 1 byte, empty, 100 MB text.
# Needs: the card running inn2x r10 and figz set up as in README.md; gzip, python3. No root, no cable.
# Run:   tools/figz/test_figz.sh [work dir]      (default tools/figz/test)
# Prints: per input "== <input>" and "  OK    <check>" / "  FAIL  <check>" lines; the helper's start-up timings; then
#   "FIGZ-TEST: N OK, N FAIL".
# PASS: 0 FAIL (exit code 0). The helper is stopped at exit.
# Takes: a few minutes.

set -u
# 1. figz, the work dir, XRT; the inputs
F=$(cd "$(dirname "$0")" && pwd)/figz; W=${1:-$(dirname "$F")/test}; mkdir -p "$W"; cd "$W"
source /opt/xilinx/xrt/setup.sh >/dev/null
python3 - <<'PY'
import glob, os
N = 32 << 20
t = b''.join(open(f, 'rb').read() for f in sorted(glob.glob('/usr/share/doc/*/copyright')) if os.path.isfile(f))
for name, data in (('text.bin', (t * (N // len(t) + 1))[:N]), ('random.bin', os.urandom(N)), ('zeros.bin', bytes(N)),
                   ('one.bin', b'x'), ('empty.bin', b''), ('big.bin', (t * (100 * N // 32 // len(t) + 1))[:100 << 20])):
    if not os.path.exists(name): open(name, 'wb').write(data)
PY
# 2. start with no helper running
trap '$F --stop' EXIT                                           # release the card (the resident helper holds it)
$F --stop
# chk <name> <test>: run the test (a shell expression), count it, print OK / FAIL
ok=0; bad=0
chk() { if eval "$2"; then ok=$((ok+1)); echo "  OK    $1"; else bad=$((bad+1)); echo "  FAIL  $1"; fi; }
# 3. per input: figz -k, gzip -t, gzip -d, figz -d, figz -dc of stock gzip -6 output, a pipe
for f in text.bin random.bin zeros.bin one.bin empty.bin big.bin; do
  rm -f $f.gz w.$f w.$f.gz; cp $f w.$f
  echo "== $f"
  $F -v -k w.$f 2>&1 | sed 's/^/    /'
  chk "figz -k keeps the input, writes w.$f.gz" "[ -f w.$f ] && [ -f w.$f.gz ]"
  chk "gzip -t accepts it" "gzip -t w.$f.gz"
  chk "gzip -d restores it" "gzip -dc w.$f.gz | cmp -s - $f"
  rm w.$f; $F -v -d w.$f.gz 2>&1 | sed 's/^/    /'
  chk "figz -d restores it (and removes the .gz)" "cmp -s w.$f $f && [ ! -e w.$f.gz ]"
  gzip -6 -c $f > c.$f.gz
  chk "figz -dc of stock gzip -6 output" "$F -dc c.$f.gz | cmp -s - $f"
  chk "pipe: cat | figz | gzip -dc" "cat $f | $F -c | gzip -dc | cmp -s - $f"
  rm -f w.$f c.$f.gz
done
# 4. corner cases
echo "== more"
cat text.bin random.bin > tr.bin; gzip -c text.bin > a.gz; gzip -c random.bin > b.gz; cat a.gz b.gz > ab.gz
chk "figz -dc of a two-member gzip file from gzip (CPU fallback)" "$F -dc ab.gz | cmp -s - tr.bin"
$F -c --chunk 8 text.bin > t8.gz
chk "--chunk 8: 4 members, gzip -d and figz -d both restore it" "gzip -dc t8.gz | cmp -s - text.bin && $F -dc t8.gz | cmp -s - text.bin"
chk "figz -t" "$F -t t8.gz"
cp one.bin x.bin; touch x.bin.gz
chk "figz refuses to overwrite an existing .gz without -f" "! $F x.bin 2>/dev/null && [ -f x.bin ] && [ ! -s x.bin.gz ]"
chk "--cpu path round trip" "$F --cpu -c random.bin | $F --cpu -dc | cmp -s - random.bin"
# short last blocks: sizes around the block boundaries figz treats specially
sbad=; for n in 1 64 65 66 67 256 257 32833 32834 32832 65537 65600 65601; do # short last blocks (see TAIL_MAX)
  head -c $n text.bin > s.bin; $F -c s.bin > s.gz; gzip -dc s.gz | cmp -s - s.bin && $F -dc s.gz | cmp -s - s.bin || sbad="$sbad $n"
done; rm -f s.bin s.gz; chk "short last blocks:${sbad:- all} sizes round trip" "[ -z \"$sbad\" ]"
cp t8.gz t8.other; chk "-c / -t ignore the suffix, as gzip" "$F -dc t8.other | cmp -s - text.bin && $F -t t8.other && ! $F -d t8.other 2>/dev/null"
rm -f t8.other; printf 'garbage' > g.gz; chk "figz -d of a non-gzip file fails" "! $F -dc g.gz > /dev/null 2>&1"
# 5. the resident helper: ms <command> = its run time in ms. The first run starts the helper, the second uses it,
#    --no-daemon runs without it
echo "== resident helper"
ms() { local s=$(date +%s%N); "$@" > /dev/null 2>&1; echo $(( ($(date +%s%N) - s) / 1000000 )); }
$F --stop; t1=$(ms $F -c one.bin); t2=$(ms $F -c one.bin); t3=$(ms $F --no-daemon -c one.bin)
echo "    1-byte file: first run ${t1} ms (starts the helper), second ${t2} ms, --no-daemon ${t3} ms"
chk "a second run through the helper takes under 150 ms" "[ $t2 -lt 150 ]"
chk "helper and --no-daemon give the same bytes" "cmp -s <($F -c text.bin) <($F --no-daemon -c text.bin)"
chk "figz -d through the helper" "$F -c text.bin | $F -dc | cmp -s - text.bin"
$F --stop; chk "figz --stop ends the helper (its socket is gone)" "[ ! -S \${XDG_RUNTIME_DIR:-\$HOME/.cache/figz}/figz-\$(id -u).sock ]"
rm -f tr.bin a.gz b.gz ab.gz t8.gz x.bin x.bin.gz g.gz
# 6. the total; the exit code is 0 only with no FAIL
echo "FIGZ-TEST: $ok OK, $bad FAIL"; [ $bad = 0 ]
