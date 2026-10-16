# graphdata — inputs and references for the graph benchmarks at AMD's sizes

AMD's graph benchmark table (`graph/docs/src/benchmark.rst`, v2023.2_update1) uses SuiteSparse graphs the library does
not ship. This directory makes them in each host's format, with independent CPU references to check the card against:

| benchmark | graph (SuiteSparse) | vertices / stored entries | reference |
|---|---|---|---|
| shortest_path_float_pred | SNAP/soc-LiveJournal1 | 4,847,571 / 68,993,773 | distances + predecessors (`sssp_ref`), weights 1.0 |
| twoHop | SNAP/soc-LiveJournal1 | same files | 1000 pairs, seed 20261016, their 2-hop counts (`twohop_ref`) |
| triangle_count | DIMACS10/europe_osm | 50,912,018 / 54,054,660 | 61,710 triangles (`tc_ref`) |
| label_propagation | LAW/hollywood-2009 | 1,139,905 / 57,515,616 | labels after 1..10 iterations (`lp_csim`) |

```
bash build.sh <Vitis_Libraries dir> <out dir> [<Vitis_HLS>/include]   # tools -> <out dir>/bin (gcc, g++)
bash prepare.sh <out dir>                         # ~1.5 GB download, 2.9 GB out, ~1 h (lp is ~5 min/iteration)
```

Copy `<out dir>/<benchmark>/` to the card's host and run the commands in `<out dir>/RUN_COMMANDS.txt` with the hosts
built by `build_hosts.sh` (triangle_count and label_propagation need the bigger arrays in `../host_patches/`).

* `mtx2csr` keeps every stored entry (no de-duplication, self-loops kept), so the edge counts equal AMD's table:
  soc-LiveJournal1 as stored (directed); europe_osm and hollywood-2009 (symmetric, stored once) with column > row
  (≥ for hollywood's self-loops), columns ascending — the orientation the triangle-count kernel requires.
* `sssp_ref`, `tc_ref` and `twohop_ref` are plain C, written from the kernels' algorithms; `sssp_csim`, `tc_csim` and
  `lp_csim` compile AMD's kernel code as C++ (HLS C-simulation semantics) on the same files — they agreed with the
  references (SSSP: 0 distance / predecessor mismatches; 61,710 triangles three ways) and the label files are
  `lp_csim`'s (the kernel's tie-break RNG is seeded inside the kernel, so its output is deterministic).
* `tc_csim` / `lp_csim` take the host's V / E and put a guard after each buffer: that is how the host array sizes in
  `host_patches/` were checked.
* soc-LiveJournal1 has no weights and AMD does not say what its run used; 1.0 on every edge makes SSSP a BFS.
* Archives: `https://www.cise.ufl.edu/research/sparse/MM/<group>/<name>.tar.gz` (the sparse.tamu.edu S3 store
  answered 403 on 2026-10-16), checked by sha256 in `prepare.sh`. The SuiteSparse Matrix Collection publishes its
  matrices under CC-BY 4.0; they are downloaded, not included in this repository.
