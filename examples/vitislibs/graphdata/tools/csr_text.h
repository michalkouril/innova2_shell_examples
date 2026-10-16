/*
 * csr_text.h -- fast readers for the Vitis graph benchmark text CSR files, parsing
 * them the same way the hosts do (first line = header, then one value per line).
 *   offsets file: header "n" (tc/lp) or "n\tn" (sssp/twoHop; the host keeps the 2nd
 *                 token), then n+1 offsets.
 *   index file:   header "nnz", then "col" or "col weight" per line.
 * Used by sssp_ref.c, twohop_ref.c, tc_ref.c and lp_csim.cpp.
 */
#ifndef CSR_TEXT_H
#define CSR_TEXT_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t n;      /* vertex count from header */
    uint64_t nnz;    /* edge count from header */
    uint32_t* off;   /* n+1 entries */
    uint32_t* col;   /* nnz entries */
    float* w;        /* nnz entries or NULL */
} csr_t;

static char* csr_slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read %s failed\n", path); exit(1); }
    b[sz] = 0; fclose(f); *len = (size_t)sz; return b;
}
/* returns pointer to start of next line */
static char* csr_nextline(char* p) { while (*p && *p != '\n') p++; return *p ? p + 1 : p; }

/* offsets: reads header (last integer token on line 1 = n), then up to n+1 values. */
static void csr_read_offsets(const char* path, csr_t* g) {
    size_t len; char* b = csr_slurp(path, &len);
    char* p = b; char* e;
    unsigned long v = 0, last = 0; int got = 0;
    char* eol = csr_nextline(p);
    while (p < eol) { v = strtoul(p, &e, 10); if (e == p) break; last = v; got++; p = e; while (*p == ' ' || *p == '\t' || *p == '\r') p++; if (*p == '\n') break; }
    if (!got) { fprintf(stderr, "%s: bad header\n", path); exit(1); }
    g->n = (uint32_t)last;
    g->off = (uint32_t*)malloc(sizeof(uint32_t) * ((size_t)g->n + 1));
    p = eol; size_t k = 0;
    while (*p) {
        v = strtoul(p, &e, 10);
        if (e == p) { p = csr_nextline(p); continue; }
        if (k > g->n) { fprintf(stderr, "%s: more than n+1 offsets\n", path); exit(1); }
        g->off[k++] = (uint32_t)v; p = csr_nextline(e);
    }
    if (k != (size_t)g->n + 1) { fprintf(stderr, "%s: got %zu offsets, expected %u\n", path, k, g->n + 1); exit(1); }
    free(b);
}
static void csr_read_index(const char* path, csr_t* g, int want_w) {
    size_t len; char* b = csr_slurp(path, &len);
    char* p = b; char* e;
    g->nnz = strtoull(p, &e, 10); p = csr_nextline(e);
    g->col = (uint32_t*)malloc(sizeof(uint32_t) * (g->nnz ? g->nnz : 1));
    g->w = want_w ? (float*)malloc(sizeof(float) * (g->nnz ? g->nnz : 1)) : NULL;
    uint64_t k = 0;
    while (*p) {
        unsigned long v = strtoul(p, &e, 10);
        if (e == p) { p = csr_nextline(p); continue; }
        if (k >= g->nnz) { fprintf(stderr, "%s: more than nnz lines\n", path); exit(1); }
        g->col[k] = (uint32_t)v;
        if (want_w) { char* e2; float f = strtof(e, &e2); g->w[k] = (e2 == e) ? 0.0f : f; e = e2; }
        k++; p = csr_nextline(e);
    }
    if (k != g->nnz) { fprintf(stderr, "%s: got %llu entries, header says %llu\n", path, (unsigned long long)k, (unsigned long long)g->nnz); exit(1); }
    if (g->off && g->off[g->n] != g->nnz) { fprintf(stderr, "WARNING: off[n]=%u != nnz=%llu\n", g->off[g->n], (unsigned long long)g->nnz); }
    for (uint64_t i = 0; i < g->nnz; i++) if (g->col[i] >= g->n) { fprintf(stderr, "%s: col %u out of range at %llu\n", path, g->col[i], (unsigned long long)i); exit(1); }
    free(b);
}
#endif
