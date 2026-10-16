/*
 * twohop_ref.c -- pair generator + CPU reference for L2/benchmarks/twoHop.
 * Kernel semantics (L2/include/hw/twoHop.hpp): for pair (s,d),
 *   cnt = sum over every out-edge s->m of (number of out-edges m->d)
 * i.e. the number of directed 2-hop walks s->m->d, multi-edges and self-loops
 * included, m may equal s or d.
 * File formats (host main.cpp): pair file = header "numPairs", then "src des" per line,
 * 1-based; golden = "src,des,count" per line, 1-based, NO header line (the host reads
 * every line of the golden into a hash map).
 * Usage:
 *   twohop_ref <offsets> <index> gen <npairs> <seed> <pairs_out> <golden_out>
 *       generates npairs UNIQUE pairs with a splitmix64 PRNG seeded by <seed>:
 *       even-numbered pairs: s,d uniform over [0,n) (mostly count 0);
 *       odd-numbered pairs: s uniform among vertices with out-degree>0, m = random
 *       out-neighbour of s, d = random out-neighbour of m (count >= 1); retried
 *       until a non-duplicate pair is found.
 *   twohop_ref <offsets> <index> use <pairs_in> <golden_out>   (count an existing pair file)
 * Build: gcc -O2 -o twohop_ref twohop_ref.c
 */
#include "csr_text.h"

static uint64_t sm_state;
static uint64_t splitmix64(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t m) { return (uint32_t)(splitmix64() % m); }

static int cmpu(const void* a, const void* b) { uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b; return x < y ? -1 : x > y; }
/* count of value d in sorted range col[a..b) */
static uint32_t count_sorted(const uint32_t* c, uint32_t a, uint32_t b, uint32_t d) {
    uint32_t lo = a, hi = b;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (c[mid] < d) lo = mid + 1; else hi = mid; }
    uint32_t k = 0; while (lo < b && c[lo] == d) { k++; lo++; }
    return k;
}

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "see header for usage\n"); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 0);
    uint32_t n = g.n;
    /* counting is order-independent; sort each row (in memory only) for binary search */
    for (uint32_t v = 0; v < n; v++) qsort(g.col + g.off[v], g.off[v + 1] - g.off[v], 4, cmpu);
    uint32_t np; uint32_t *S, *D; const char* gold;
    if (!strcmp(argv[3], "gen")) {
        if (argc != 8) { fprintf(stderr, "gen needs npairs seed pairs_out golden_out\n"); return 2; }
        np = (uint32_t)strtoul(argv[4], 0, 10); sm_state = strtoull(argv[5], 0, 10); gold = argv[7];
        S = malloc(4 * (size_t)np); D = malloc(4 * (size_t)np);
        uint32_t* nz = malloc(4 * (size_t)n); uint32_t nnzv = 0;
        for (uint32_t v = 0; v < n; v++) if (g.off[v + 1] > g.off[v]) nz[nnzv++] = v;
        for (uint32_t i = 0; i < np; i++) {
            for (;;) {
                uint32_t s, d;
                if (i % 2 == 0) { s = rnd(n); d = rnd(n); }
                else {
                    s = nz[rnd(nnzv)];
                    uint32_t m = g.col[g.off[s] + rnd(g.off[s + 1] - g.off[s])];
                    if (g.off[m + 1] == g.off[m]) continue;
                    d = g.col[g.off[m] + rnd(g.off[m + 1] - g.off[m])];
                }
                int dup = 0;
                for (uint32_t j = 0; j < i; j++) if (S[j] == s && D[j] == d) { dup = 1; break; }
                if (dup) continue;
                S[i] = s; D[i] = d; break;
            }
        }
        FILE* fp = fopen(argv[6], "w"); if (!fp) { perror(argv[6]); return 1; }
        fprintf(fp, "%u\n", np);
        for (uint32_t i = 0; i < np; i++) fprintf(fp, "%u %u\n", S[i] + 1, D[i] + 1);
        if (fclose(fp)) { perror("close"); return 1; }
    } else if (!strcmp(argv[3], "use")) {
        gold = argv[5];
        FILE* fp = fopen(argv[4], "r"); if (!fp) { perror(argv[4]); return 1; }
        if (fscanf(fp, "%u", &np) != 1) return 1;
        S = malloc(4 * (size_t)np); D = malloc(4 * (size_t)np);
        for (uint32_t i = 0; i < np; i++) {
            unsigned a, b; if (fscanf(fp, "%u %u", &a, &b) != 2) { fprintf(stderr, "short pair file\n"); return 1; }
            S[i] = a - 1; D[i] = b - 1;
        }
        fclose(fp);
    } else { fprintf(stderr, "mode gen|use\n"); return 2; }
    FILE* fo = fopen(gold, "w"); if (!fo) { perror(gold); return 1; }
    uint64_t total = 0, work = 0, maxwork = 0; uint32_t nonzero = 0, maxcnt = 0;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t s = S[i], d = D[i]; uint64_t c = 0, w = 0;
        for (uint32_t e = g.off[s]; e < g.off[s + 1]; e++) {
            uint32_t m = g.col[e];
            w += g.off[m + 1] - g.off[m];
            c += count_sorted(g.col, g.off[m], g.off[m + 1], d);
        }
        if (c > 0xffffffffULL) { fprintf(stderr, "count overflows 32 bit\n"); return 1; }
        fprintf(fo, "%u,%u,%llu\n", s + 1, d + 1, (unsigned long long)c);
        total += c; if (c) nonzero++; if (c > maxcnt) maxcnt = (uint32_t)c;
        work += w; if (w > maxwork) maxwork = w;
    }
    if (fclose(fo)) { perror("close"); return 1; }
    printf("n=%u nnz=%llu pairs=%u nonzero=%u sum_counts=%llu max_count=%u\n", n,
           (unsigned long long)g.nnz, np, nonzero, (unsigned long long)total, maxcnt);
    printf("kernel work: 2-hop index reads total=%llu max_per_pair=%llu\n",
           (unsigned long long)work, (unsigned long long)maxwork);
    return 0;
}
