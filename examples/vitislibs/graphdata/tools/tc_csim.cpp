/*
 * tc_csim.cpp -- runs AMD's own triangle-count kernel code (xf::graph::preProcessData +
 * xf::graph::triangleCount<LEN=128, ML=65536>, as kernel/TC_kernel.cpp calls them) in
 * C-simulation on the host's csr_offsets / csr_columns files. Buffers are sized like the
 * host does with the V/E it is compiled with (offsets: V, rows: E, offsets2: 2V DT words)
 * plus a 1 MiB canary guard each, checked afterwards. Cross-checks tc_ref.
 * Usage: tc_csim <csr_offsets> <csr_columns> <Vhost> <Ehost>
 * Build: ../build.sh (with the library tree and the Vitis HLS headers)
 */
#include <ap_int.h>
#include <hls_stream.h>
#include "hw/triangle_count.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "csr_text.h"
}
static const size_t GUARD = 1 << 20;
static unsigned char* mk(size_t bytes) {
    void* p; if (posix_memalign(&p, 4096, bytes + GUARD)) exit(1);
    memset(p, 0, bytes); memset((char*)p + bytes, 0xA5, GUARD); return (unsigned char*)p;
}
static int ok(unsigned char* p, size_t bytes, const char* nm) {
    for (size_t i = 0; i < GUARD; i++) if (p[bytes + i] != 0xA5) { printf("GUARD DAMAGED %s +%zu\n", nm, i); return 0; }
    return 1;
}
int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s offsets columns Vhost Ehost\n", argv[0]); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 0);
    size_t Vh = strtoull(argv[3], 0, 10), Eh = strtoull(argv[4], 0, 10);
    if (Vh < (size_t)g.n + 1 || Eh < g.nnz) { fprintf(stderr, "host V/E too small for this file\n"); return 1; }
    unsigned char* off = mk(4 * Vh); unsigned char* rows = mk(4 * Eh); unsigned char* off2 = mk(8 * Vh);
    for (uint32_t i = 0; i <= g.n; i++) ((uint32_t*)off)[i] = g.off[i];
    for (uint64_t i = 0; i < g.nnz; i++) ((uint32_t*)rows)[i] = g.col[i];
    free(g.col); free(g.off);
    uint64_t TC[1] = {0};
    int vertexNum = (int)g.n, edgeNum = (int)g.nnz;
    typedef ap_uint<512> u5;
    /* == TC_kernel body == (host passes the same offsets/rows buffers for G1/G2/G3 rows) */
    xf::graph::preProcessData(vertexNum, (u5*)off, (u5*)off2);
    xf::graph::triangleCount<128, 65536>(vertexNum, edgeNum, (u5*)off, (u5*)rows, (u5*)off, (u5*)rows, (u5*)off2,
                                         (u5*)rows, TC);
    int good = ok(off, 4 * Vh, "offsets") & ok(rows, 4 * Eh, "rows") & ok(off2, 8 * Vh, "offsets2");
    printf("csim triangles=%llu guards %s\n", (unsigned long long)TC[0], good ? "intact" : "DAMAGED");
    return good ? 0 : 3;
}
