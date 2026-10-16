/*
 * lp_csim.cpp -- bit-exact CPU reference for L2/benchmarks/label_propagation, made by
 * running AMD's own kernel source (xf::graph::convertCsrCsc + xf::graph::labelPropagation,
 * exactly as kernel/label_propagation_top.cpp calls them) as plain C++ (HLS C-simulation
 * semantics, no __SYNTHESIS__). LP is deterministic: the kernel's tie-break RNG is
 * xoshiro128++ seeded {1,2,3,4} + jump() inside the kernel, so csim output == hardware
 * output for the same input and iteration count.
 * Buffers are allocated the way the host does (sizeof(DT)*V or *E bytes, with the V/E the
 * host is compiled with, given as arguments) plus a 1 MiB canary guard after each, which is
 * checked afterwards: a damaged guard means the host's buffer sizes are too small.
 * Output: label file in the host's -label format: header "n n", then "vertex label"
 * (0-based) per line; the host compares labelPong when iter is odd, labelPing when even.
 * Env LP_DUMP_EACH=<prefix>: also write <prefix><k>.txt after each iteration k (see code).
 * Usage: lp_csim <csr_offsets> <csr_columns> <iter> <Vhost> <Ehost> <label_out> [golden_in]
 * Build: ../build.sh (needs the Vitis HLS ap_int / hls_stream headers and the library tree).
 */
#include <ap_int.h>
#include <hls_stream.h>
#include "hw/convert_csr_csc.hpp"
#include "hw/label_propagation.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "csr_text.h"
}

typedef ap_uint<32> DT;
typedef ap_uint<512> uint512;
#define TMPL_V 800000 /* only used as array-parameter sizes; same values as the shipped kernel */
#define TMPL_E 800000
static const size_t GUARD = 1 << 20;

struct Buf {
    unsigned char* p; size_t bytes; const char* name;
};
static Buf mk(const char* name, size_t bytes) {
    Buf b; b.name = name; b.bytes = bytes;
    void* p = nullptr;
    if (posix_memalign(&p, 4096, bytes + GUARD)) { fprintf(stderr, "oom %s\n", name); exit(1); }
    b.p = (unsigned char*)p; memset(b.p, 0, bytes); memset(b.p + bytes, 0xA5, GUARD);
    return b;
}
static int guard_ok(const Buf& b) {
    for (size_t i = 0; i < GUARD; i++) if (b.p[b.bytes + i] != 0xA5) {
        fprintf(stderr, "GUARD DAMAGED: %s (host size %zu bytes) written at +%zu\n", b.name, b.bytes, i); return 0; }
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s offsets columns iter Vhost Ehost label_out [golden]\n", argv[0]); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 0);
    int iter = atoi(argv[3]);
    size_t Vh = strtoull(argv[4], 0, 10), Eh = strtoull(argv[5], 0, 10);
    int vertexNum = (int)g.n, edgeNum = (int)g.nnz;
    if (Vh < (size_t)vertexNum + 1 || Eh < (size_t)edgeNum) { fprintf(stderr, "Vhost/Ehost smaller than the file: host would overflow\n"); return 1; }
    Buf offCSR = mk("offsetsCSR", 4 * Vh), colCSR = mk("columnsCSR", 4 * Eh), offCSC = mk("offsetsCSC", 4 * Vh),
        rowCSC = mk("rowsCSC", 4 * Eh), bPing = mk("bufPing", 4 * Vh), bPong = mk("bufPong", 4 * Vh),
        lPing = mk("labelPing", 4 * Vh), lPong = mk("labelPong", 4 * Vh);
    uint32_t* o = (uint32_t*)offCSR.p; for (uint32_t i = 0; i <= g.n; i++) o[i] = g.off[i];
    uint32_t* c = (uint32_t*)colCSR.p; for (uint64_t i = 0; i < g.nnz; i++) c[i] = g.col[i];
    free(g.col); g.col = nullptr;
    fprintf(stderr, "vertexNum=%d edgeNum=%d iter=%d Vhost=%zu Ehost=%zu\n", vertexNum, edgeNum, iter, Vh, Eh);
    /* == LPKernel body (kernel/label_propagation_top.cpp) == */
    const int cacheDepthBin = 0, dataOneLineBin = 4, usURAM = 0;
    xf::graph::convertCsrCsc<DT, TMPL_V, TMPL_E, cacheDepthBin, dataOneLineBin, usURAM>(
        edgeNum, vertexNum, (uint512*)offCSR.p, (uint512*)colCSR.p, (uint512*)offCSC.p, (DT*)rowCSC.p,
        (uint512*)lPing.p, (uint512*)lPong.p);
    fprintf(stderr, "convertCsrCsc done\n");
    if (!getenv("LP_DUMP_EACH")) {
        xf::graph::labelPropagation(edgeNum, vertexNum, iter, (uint512*)offCSR.p, (uint512*)colCSR.p,
                                    (uint512*)offCSC.p, (uint512*)rowCSC.p, (uint512*)bPing.p, (uint512*)bPong.p,
                                    (uint512*)lPing.p, (uint512*)lPong.p);
    } else {
        /* LP_DUMP_EACH=<prefix>: line-for-line copy of xf::graph::labelPropagation's body
           (label_propagation.hpp ~1650-1666) that also writes <prefix><k>.txt after every
           iteration k, so one run yields the golden for -iter 1..iter. */
        const char* pre = getenv("LP_DUMP_EACH");
        xf::graph::internal::label_propagation::labelInit<ap_uint<32>, ap_uint<512>, 16, 32>(vertexNum, (uint512*)lPing.p);
        xf::fintech::XoShiRo128PlusPlus rng;
        unsigned int seed[4] = {1, 2, 3, 4};
        rng.init(seed);
        rng.jump();
        for (int i = 0; i < iter; i++) {
            if (i % 2 == 0)
                xf::graph::internal::label_propagation::lpCoreImpl<ap_uint<32>, ap_uint<512>, 16, 32, xf::fintech::XoShiRo128PlusPlus>(
                    edgeNum, vertexNum, (uint512*)colCSR.p, (uint512*)offCSR.p, (uint512*)rowCSC.p, (uint512*)offCSC.p,
                    (uint512*)bPing.p, (uint512*)bPong.p, (uint512*)lPing.p, (uint512*)lPong.p, rng);
            else
                xf::graph::internal::label_propagation::lpCoreImpl<ap_uint<32>, ap_uint<512>, 16, 32, xf::fintech::XoShiRo128PlusPlus>(
                    edgeNum, vertexNum, (uint512*)colCSR.p, (uint512*)offCSR.p, (uint512*)rowCSC.p, (uint512*)offCSC.p,
                    (uint512*)bPing.p, (uint512*)bPong.p, (uint512*)lPong.p, (uint512*)lPing.p, rng);
            uint32_t* L = (uint32_t*)(((i + 1) % 2) ? lPong.p : lPing.p);
            char nm[1024]; snprintf(nm, sizeof nm, "%s%d.txt", pre, i + 1);
            FILE* f = fopen(nm, "w"); if (!f) { perror(nm); return 1; }
            fprintf(f, "%d %d\n", vertexNum, vertexNum);
            uint64_t hh = 1469598103934665603ULL; uint64_t changed = 0;
            uint32_t* P = (uint32_t*)(((i + 1) % 2) ? lPing.p : lPong.p); /* previous labels */
            for (int v = 0; v < vertexNum; v++) { fprintf(f, "%d %u\n", v, L[v]); hh = (hh ^ L[v]) * 1099511628211ULL; if (i > 0 && L[v] != P[v]) changed++; }
            if (fclose(f)) { perror("close"); return 1; }
            printf("iter %d: wrote %s fnv64=%016llx changed_vs_prev=%llu\n", i + 1, nm, (unsigned long long)hh, (unsigned long long)changed);
            fflush(stdout);
        }
    }
    fprintf(stderr, "labelPropagation done\n");
    int ok = 1;
    const Buf* all[] = {&offCSR, &colCSR, &offCSC, &rowCSC, &bPing, &bPong, &lPing, &lPong};
    for (auto b : all) ok &= guard_ok(*b);
    printf("guards %s\n", ok ? "intact (host buffer sizes sufficient)" : "DAMAGED");
    /* CSC sanity: offsetsCSC[n] must equal edgeNum */
    printf("offsetsCSC[n]=%u (expect %d)\n", ((uint32_t*)offCSC.p)[vertexNum], edgeNum);
    uint32_t* lab = (uint32_t*)((iter % 2) ? lPong.p : lPing.p);
    FILE* fo = fopen(argv[6], "w"); if (!fo) { perror(argv[6]); return 1; }
    fprintf(fo, "%d %d\n", vertexNum, vertexNum);
    uint64_t self = 0, h = 1469598103934665603ULL;
    std::vector<uint32_t> cnt(vertexNum, 0); uint32_t distinct = 0;
    for (int i = 0; i < vertexNum; i++) {
        fprintf(fo, "%d %u\n", i, lab[i]);
        if (lab[i] == (uint32_t)i) self++;
        if (lab[i] < (uint32_t)vertexNum && cnt[lab[i]]++ == 0) distinct++;
        h = (h ^ lab[i]) * 1099511628211ULL;
    }
    if (fclose(fo)) { perror("close"); return 1; }
    printf("labels: distinct=%u keep-own-id=%llu fnv64=%016llx\n", distinct, (unsigned long long)self, (unsigned long long)h);
    if (argc > 7) {
        FILE* fg = fopen(argv[7], "r"); if (!fg) { perror(argv[7]); return 1; }
        char line[256]; int idx = 0, bad = 0;
        while (fgets(line, sizeof line, fg)) {
            unsigned a, b; if (sscanf(line, "%u %u", &a, &b) != 2) continue;
            if (idx > 0 && idx - 1 < vertexNum && lab[idx - 1] != b) bad++;
            idx++;
        }
        fclose(fg);
        printf("compare with %s: %d mismatches over %d vertices\n", argv[7], bad, idx - 1);
    }
    return ok ? 0 : 3;
}
