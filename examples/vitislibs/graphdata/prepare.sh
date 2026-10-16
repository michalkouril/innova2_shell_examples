#!/bin/bash
# SPDX-FileCopyrightText: 2026 the innova2 contributors
#
# SPDX-License-Identifier: Apache-2.0

# prepare.sh <out dir> [step ...] -- the inputs and CPU references for the four graph benchmarks at the sizes AMD
# publishes (graph/docs/src/benchmark.rst), after build.sh <Vitis_Libraries dir> <out dir> (tools in <out dir>/bin). Steps (default all): fetch convert sssp twohop tc lp.
#   fetch    SuiteSparse archives (the UF mirror: sparse.tamu.edu answered 403 on 2026-10-16), checked by sha256
#   convert  Matrix Market -> each host's text CSR (every stored entry kept, so edge counts equal AMD's table)
#   sssp     shortest_path_float_pred, soc-LiveJournal1: golden from sssp_ref (weights 1.0; source = the host's)
#   twohop   twoHop, soc-LiveJournal1: 1000 pairs (splitmix64 seed 20261016) and their counts
#   tc       triangle_count, europe_osm: the count (tc_ref; expected 61710), for the host's TC_GOLDEN
#   lp       label_propagation, hollywood-2009: the labels after 1..10 iterations (lp_csim, AMD's kernel as C++;
#            ~5 min per iteration, ~2 GB)
# Out: <out dir>/<benchmark>/ (2.9 GB in all), with the host command lines in <out dir>/RUN_COMMANDS.txt.
set -eu
O=$(mkdir -p "${1:?usage: $0 <out dir> [step ...]}" && readlink -f "$1"); shift
B=$O/bin; [ -x $B/mtx2csr ] || { echo "*** run build.sh <Vitis_Libraries dir> $O first"; exit 2; }
STEPS=${*:-fetch convert sssp twohop tc lp}
U=https://www.cise.ufl.edu/research/sparse/MM
declare -A SHA=([SNAP/soc-LiveJournal1]=6518202e02a60f4db9828f3f6ce756191436e61c061c44b66e869b7ba28a647b
                [DIMACS10/europe_osm]=22cfb3e6bf0c89193414c4310dbcf51266632c943c420728386d8ebaee1282af
                [LAW/hollywood-2009]=5b6e9cebbb17c702b596f68da23d50cbbbb0f9b6c977abffb018c91200944460)
mtx() { echo $O/dl/$(basename $1)/$(basename $1).mtx; }
for s in $STEPS; do case $s in
  fetch) for m in "${!SHA[@]}"; do d=$O/dl/$(basename $m); mkdir -p $d; a=$d/$(basename $m).tar.gz
           [ -s $a ] || curl -fsSL -o $a $U/$m.tar.gz
           echo "${SHA[$m]}  $a" | sha256sum -c --quiet || { echo "*** $a: sha256 mismatch"; exit 1; }
           [ -s $(mtx $m) ] || tar -xzf $a -C $O/dl; echo "fetched $m"; done ;;
  convert) S=$O/shortest_path_float_pred; T=$O/twoHop; C=$O/triangle_count; L=$O/label_propagation; mkdir -p $S $T $C $L
           $B/mtx2csr $(mtx SNAP/soc-LiveJournal1) asis sssp $S/lj-csr-offset.mtx $S/lj-csr-indicesweights.mtx
           ln -f $S/lj-csr-offset.mtx $T/lj-csr-offset.mtx; ln -f $S/lj-csr-indicesweights.mtx $T/lj-csr-indicesweights.mtx
           $B/mtx2csr $(mtx DIMACS10/europe_osm) upper tc $C/europe_osm_csr_offsets.txt $C/europe_osm_csr_columns.txt
           $B/mtx2csr $(mtx LAW/hollywood-2009) upper tc $L/hollywood_csr_offsets.txt $L/hollywood_csr_columns.txt ;;
  sssp)    cd $O/shortest_path_float_pred && $B/sssp_ref lj-csr-offset.mtx lj-csr-indicesweights.mtx lj-golden.sssp.mtx ;;
  twohop)  cd $O/twoHop && $B/twohop_ref lj-csr-offset.mtx lj-csr-indicesweights.mtx gen 1000 20261016 lj-pair.mtx lj-golden.twoHop.mtx ;;
  tc)      cd $O/triangle_count && $B/tc_ref europe_osm_csr_offsets.txt europe_osm_csr_columns.txt ;;
  lp)      cd $O/label_propagation && LP_DUMP_EACH=hollywood_label_iter $B/lp_csim hollywood_csr_offsets.txt \
             hollywood_csr_columns.txt 10 1140000 57600000 hollywood_label_final10.txt ;;
  *) echo "*** unknown step $s"; exit 2 ;;
esac; done
cat > $O/RUN_COMMANDS.txt <<'RC'
# on the card's host, from <out dir>/<benchmark>/, with that benchmark's host and xclbin (examples/vitislibs):
shortest_path_float_pred: host.exe -xclbin <x> -o lj-csr-offset.mtx -c lj-csr-indicesweights.mtx -g lj-golden.sssp.mtx
twoHop: host.exe -xclbin <x> --offset lj-csr-offset.mtx --index lj-csr-indicesweights.mtx --pair lj-pair.mtx --golden lj-golden.twoHop.mtx
triangle_count: TC_GOLDEN=61710 host.exe -xclbin <x> -o europe_osm_csr_offsets.txt -i europe_osm_csr_columns.txt
label_propagation: host.exe -xclbin <x> -o hollywood_csr_offsets.txt -i hollywood_csr_columns.txt -iter <k> -label hollywood_label_iter<k>.txt
RC
echo "PREPARE-DONE $O"
