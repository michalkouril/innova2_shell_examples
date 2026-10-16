/*
 * tc_ref.c -- CPU reference for L2/benchmarks/triangle_count.
 * Input: the host's csr_offsets / csr_columns text files (header "n" / "nnz").
 * The kernel (L2/include/hw/triangle_count.hpp) is the forward algorithm on an
 * upper-triangular CSR: for each u and each i-th neighbour v = adj(u)[i], it
 * intersects adj(u)[i+1..] with adj(v) using orderStrmInterNum (both lists must be
 * ascending). This program
 *   1. checks the input is what that algorithm needs: every column > its row,
 *      strictly ascending within a row, and reports max out-degree vs ML = 65536;
 *   2. counts with a plain merge intersection, and
 *   3. counts again with a literal replay of orderStrmInterNum (initial value 0
 *      sentinel included) so any quirk of the kernel's merge would show up as a
 *      difference.
 * Usage: tc_ref <csr_offsets> <csr_columns>      Build: gcc -O2 -o tc_ref tc_ref.c
 */
#include "csr_text.h"

/* literal replay of xf::graph::internal::triangle_count::orderStrmInterNum */
static int inter_kernel(const uint32_t* a, uint32_t na, const uint32_t* b, uint32_t nb) {
    uint32_t ia = 0, ib = 0;
    int end1 = (na == 0), end2 = (nb == 0); /* end flag read before the first value */
    uint32_t v1 = 0, v2 = 0; int num = 0;
#define RD1() do { v1 = a[ia++]; end1 = (ia >= na); } while (0)
#define RD2() do { v2 = b[ib++]; end2 = (ib >= nb); } while (0)
    while (!end1 || !end2) {
        if (v1 == v2) { if (!end1) RD1(); if (!end2) RD2(); }
        else if (v1 > v2) { if (!end2) RD2(); else RD1(); }
        else { if (!end1) RD1(); else RD2(); }
        if (v1 == v2) num++;
    }
    return num;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s csr_offsets csr_columns\n", argv[0]); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 0);
    uint32_t n = g.n, maxd = 0, maxv = 0;
    uint64_t notupper = 0, notsorted = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t d = g.off[u + 1] - g.off[u];
        if (d > maxd) { maxd = d; maxv = u; }
        for (uint32_t e = g.off[u]; e < g.off[u + 1]; e++) {
            if (g.col[e] <= u) notupper++;
            if (e > g.off[u] && g.col[e] <= g.col[e - 1]) notsorted++;
        }
    }
    printf("n=%u nnz=%llu max_out=%u at %u (ML=65536) col<=row entries=%llu non-ascending=%llu\n", n,
           (unsigned long long)g.nnz, maxd, maxv, (unsigned long long)notupper, (unsigned long long)notsorted);
    uint64_t tc = 0, tck = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t b = g.off[u], e = g.off[u + 1];
        for (uint32_t i = b; i < e; i++) {
            uint32_t v = g.col[i];
            const uint32_t* A = g.col + i + 1; uint32_t na = e - i - 1;
            const uint32_t* B = g.col + g.off[v]; uint32_t nb = g.off[v + 1] - g.off[v];
            uint32_t x = 0, y = 0;
            while (x < na && y < nb) {
                if (A[x] < B[y]) x++; else if (A[x] > B[y]) y++; else { tc++; x++; y++; }
            }
            /* the kernel only issues an intersection when copyNum (= degree of u) > 1 */
            if (e - b > 1) tck += inter_kernel(A, na, B, nb);
        }
    }
    printf("triangles (merge)=%llu  triangles (kernel-merge replay)=%llu\n", (unsigned long long)tc, (unsigned long long)tck);
    return 0;
}
