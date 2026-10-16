/*
 * mtx2csr.c -- convert a SuiteSparse Matrix Market "coordinate pattern" file
 * into the text CSR formats read by the Vitis Libraries graph L2 benchmark hosts.
 *
 * Usage: mtx2csr <in.mtx> <orient> <fmt> <out_offsets> <out_columns>
 *   orient: asis  -> CSR row = MM row-1, col = MM col-1 (directed edge i->j as stored)
 *           upper -> each stored entry (i,j) becomes row=min, col=max (each undirected
 *                    edge once, columns > row; diagonal entries kept as row==col)
 *   fmt:    sssp  -> offsets: "n\tn" header + n+1 lines;  columns: "nnz" header + "col\t1"
 *                    (shortest_path_float_pred / twoHop format, weight 1 on every edge)
 *           tc    -> offsets: "n" header + n+1 lines;     columns: "nnz" header + "col"
 *                    (triangle_count / label_propagation format)
 * Every entry is kept (no dedup, no self-loop removal) so the edge count equals the
 * MM nnz line, which is what AMD's benchmark table lists. Columns are sorted
 * ascending within each row. Duplicates/self-loops are counted and reported.
 * Build: gcc -O2 -o mtx2csr mtx2csr.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static FILE* fin;
static int rdc(void) { return getc_unlocked(fin); }
static int64_t rdnum(int* eof) {
    int c = rdc();
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = rdc();
    if (c == EOF) { *eof = 1; return -1; }
    int64_t v = 0;
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = rdc(); }
    while (c != '\n' && c != EOF) c = rdc(); /* rest of line ignored (pattern: none) */
    return v;
}
static int64_t rdnum_inline(int* eof) { /* number not ending the line */
    int c = rdc();
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = rdc();
    if (c == EOF) { *eof = 1; return -1; }
    int64_t v = 0;
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = rdc(); }
    return v;
}
static int cmpu(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}
static char* obuf; static size_t opos; static FILE* fo;
#define OBSZ (1 << 22)
static void oflush(void) { fwrite(obuf, 1, opos, fo); opos = 0; }
static void ostr(const char* s) { while (*s) { if (opos >= OBSZ) oflush(); obuf[opos++] = *s++; } }
static void onum(uint64_t v) {
    char t[24]; int k = 0;
    do { t[k++] = '0' + v % 10; v /= 10; } while (v);
    if (opos + 24 >= OBSZ) oflush();
    while (k) obuf[opos++] = t[--k];
}

int main(int argc, char** argv) {
    if (argc != 6) { fprintf(stderr, "usage: %s in.mtx asis|upper sssp|tc out_off out_col\n", argv[0]); return 2; }
    int upper = !strcmp(argv[2], "upper");
    if (!upper && strcmp(argv[2], "asis")) { fprintf(stderr, "bad orient\n"); return 2; }
    int sssp = !strcmp(argv[3], "sssp");
    if (!sssp && strcmp(argv[3], "tc")) { fprintf(stderr, "bad fmt\n"); return 2; }
    fin = fopen(argv[1], "r");
    if (!fin) { perror(argv[1]); return 1; }
    static char line[4096];
    if (!fgets(line, sizeof line, fin) || strncmp(line, "%%MatrixMarket matrix coordinate pattern", 40)) {
        fprintf(stderr, "not a coordinate pattern MM file: %s", line); return 1;
    }
    int sym = strstr(line, "symmetric") != NULL;
    fprintf(stderr, "header: %s", line);
    int c;
    while ((c = rdc()) == '%') { while ((c = rdc()) != '\n' && c != EOF) ; }
    ungetc(c, fin);
    int eof = 0;
    int64_t nr = rdnum_inline(&eof), nc = rdnum_inline(&eof), nnz = rdnum(&eof);
    fprintf(stderr, "rows %lld cols %lld nnz %lld symmetric %d\n", (long long)nr, (long long)nc, (long long)nnz, sym);
    if (nr != nc || nr <= 0 || nr > 0x7fffffff || nnz <= 0 || nnz > 0xffffffffLL) { fprintf(stderr, "bad dims\n"); return 1; }
    uint32_t n = (uint32_t)nr;
    uint32_t* R = malloc(sizeof(uint32_t) * nnz);
    uint32_t* C = malloc(sizeof(uint32_t) * nnz);
    if (!R || !C) { fprintf(stderr, "oom\n"); return 1; }
    int64_t selfl = 0, lowercnt = 0, uppercnt = 0;
    for (int64_t k = 0; k < nnz; k++) {
        int64_t i = rdnum_inline(&eof), j = rdnum(&eof);
        if (eof || i < 1 || j < 1 || i > nr || j > nr) { fprintf(stderr, "bad entry %lld (%lld,%lld)\n", (long long)k, (long long)i, (long long)j); return 1; }
        uint32_t a = (uint32_t)(i - 1), b = (uint32_t)(j - 1);
        if (a == b) selfl++; else if (a > b) lowercnt++; else uppercnt++;
        if (upper && a > b) { uint32_t t = a; a = b; b = t; }
        R[k] = a; C[k] = b;
    }
    { int64_t extra = rdnum(&eof); if (!eof) { fprintf(stderr, "trailing data after nnz entries (%lld)\n", (long long)extra); return 1; } }
    fclose(fin);
    fprintf(stderr, "entries: diag %lld, below-diag %lld, above-diag %lld\n", (long long)selfl, (long long)lowercnt, (long long)uppercnt);
    uint32_t* off = calloc((size_t)n + 1, sizeof(uint32_t));
    if (!off) { fprintf(stderr, "oom\n"); return 1; }
    for (int64_t k = 0; k < nnz; k++) off[R[k] + 1]++;
    uint32_t maxd = 0, maxv = 0, zero = 0;
    for (uint32_t v = 0; v < n; v++) {
        if (off[v + 1] > maxd) { maxd = off[v + 1]; maxv = v; }
        if (off[v + 1] == 0) zero++;
    }
    for (uint32_t v = 0; v < n; v++) off[v + 1] += off[v];
    uint32_t* pos = malloc(sizeof(uint32_t) * n);
    if (!pos) { fprintf(stderr, "oom\n"); return 1; }
    memcpy(pos, off, sizeof(uint32_t) * n);
    uint32_t* col = malloc(sizeof(uint32_t) * nnz);
    if (!col) { fprintf(stderr, "oom\n"); return 1; }
    for (int64_t k = 0; k < nnz; k++) col[pos[R[k]]++] = C[k];
    free(R); free(C); free(pos);
    int64_t dups = 0;
    for (uint32_t v = 0; v < n; v++) {
        qsort(col + off[v], off[v + 1] - off[v], sizeof(uint32_t), cmpu);
        for (uint32_t e = off[v] + 1; e < off[v + 1]; e++) if (col[e] == col[e - 1]) dups++;
    }
    fprintf(stderr, "CSR: n %u nnz %lld, max out-degree %u (first at vertex %u, 0-based), zero-out-degree vertices %u, duplicate entries %lld\n",
            n, (long long)nnz, maxd, maxv, zero, (long long)dups);
    obuf = malloc(OBSZ);
    if (!obuf) { fprintf(stderr, "oom\n"); return 1; }
    fo = fopen(argv[4], "w"); if (!fo) { perror(argv[4]); return 1; }
    onum(n); if (sssp) { ostr("\t"); onum(n); } ostr("\n");
    for (uint32_t v = 0; v <= n; v++) { onum(off[v]); ostr("\n"); }
    oflush(); if (fclose(fo)) { perror("close"); return 1; }
    fo = fopen(argv[5], "w"); if (!fo) { perror(argv[5]); return 1; }
    onum((uint64_t)nnz); ostr("\n");
    for (int64_t e = 0; e < nnz; e++) { onum(col[e]); if (sssp) ostr("\t1"); ostr("\n"); }
    oflush(); if (fclose(fo)) { perror("close"); return 1; }
    fprintf(stderr, "wrote %s %s\n", argv[4], argv[5]);
    return 0;
}
