/*
 * sssp_csim.cpp -- runs AMD's own SSSP kernel code (xf::graph::singleSourceShortestPath
 * <32, MAXOUTDEGREE=40960>, as kernel/shortestPath_top.cpp calls it) in C-simulation on
 * the host's input files, with the host's config words (float, weights on, pred on,
 * qcfg = 10*300*4096, source = first vertex of max out-degree), and compares the
 * result with a golden file written by sssp_ref (distances AND predecessors, exact).
 * This cross-checks that sssp_ref replays the kernel faithfully.
 * Usage: sssp_csim <offsets> <indicesweights> <golden_from_sssp_ref>
 * Build: ../build.sh (with the library tree and the Vitis HLS headers)
 */
#include <ap_int.h>
#include <hls_stream.h>
#include "hw/shortest_path.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <vector>
extern "C" {
#include "csr_text.h"
}
template <class T> static T* al(size_t n) { void* p; if (posix_memalign(&p, 4096, n * sizeof(T) + 64)) exit(1); memset(p, 0, n * sizeof(T) + 64); return (T*)p; }

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s offsets indicesweights golden\n", argv[0]); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 1);
    uint32_t n = g.n; size_t nr = ((n + 1023) / 1024) * 1024;
    ap_uint<32>* offset32 = al<ap_uint<32> >(n + 1 + 16);
    for (uint32_t i = 0; i <= n; i++) offset32[i] = g.off[i];
    ap_uint<32>* column32 = al<ap_uint<32> >(g.nnz + 16);
    float* weight32 = al<float>(g.nnz + 16);
    for (uint64_t i = 0; i < g.nnz; i++) { column32[i] = g.col[i]; weight32[i] = g.w[i]; }
    uint32_t src = 0, maxd = 0;
    for (uint32_t i = 0; i < n; i++) if (g.off[i + 1] - g.off[i] > maxd) { maxd = g.off[i + 1] - g.off[i]; src = i; }
    free(g.col); free(g.w);
    ap_uint<8>* info = al<ap_uint<8> >(4);
    float* result = al<float>(nr);
    ap_uint<32>* pred = al<ap_uint<32> >(nr);
    ap_uint<32>* ddrQue = al<ap_uint<32> >(10 * 300 * 4096);
    ap_uint<32>* config = al<ap_uint<32> >(6);
    config[0] = n;
    float inf = std::numeric_limits<float>::infinity(); uint32_t infb; memcpy(&infb, &inf, 4);
    config[1] = infb; config[2] = 0; config[3] = 10 * 300 * 4096;
    ap_uint<32> cmd = 0; cmd.set_bit(0, 1); cmd.set_bit(1, 1); cmd.set_bit(2, 0);
    config[4] = cmd; config[5] = src;
    fprintf(stderr, "csim SSSP n=%u nnz=%llu source=%u\n", n, (unsigned long long)g.nnz, src);
    xf::graph::singleSourceShortestPath<32, 4096 * 10>(
        config, (ap_uint<512>*)offset32, (ap_uint<512>*)column32, (ap_uint<512>*)weight32, (ap_uint<512>*)ddrQue,
        ddrQue, (ap_uint<512>*)result, (ap_uint<32>*)result, (ap_uint<512>*)pred, pred, info);
    printf("info: queue_overflow=%d table_overflow=%d\n", (int)info[0], (int)info[1]);
    /* compare with golden (1-based "v,dist,pred"), and require unlisted vertices unreachable */
    std::vector<char> listed(n, 0);
    FILE* f = fopen(argv[3], "r"); if (!f) { perror(argv[3]); return 1; }
    char line[256]; fgets(line, sizeof line, f);
    uint64_t dbad = 0, pbad = 0, cnt = 0, ubad = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned v, p; float d;
        if (sscanf(line, "%u,%f,%u", &v, &d, &p) != 3) continue;
        cnt++; listed[v - 1] = 1;
        if (result[v - 1] != d) dbad++;
        if ((uint32_t)pred[v - 1] != p - 1) pbad++;
    }
    fclose(f);
    for (uint32_t i = 0; i < n; i++) if (!listed[i] && (result[i] != inf || (uint32_t)pred[i] != 0xFFFFFFFFu)) ubad++;
    printf("golden rows=%llu dist mismatches=%llu pred mismatches=%llu unreachable mismatches=%llu\n",
           (unsigned long long)cnt, (unsigned long long)dbad, (unsigned long long)pbad, (unsigned long long)ubad);
    return (dbad || pbad || ubad || info[0] || info[1]) ? 1 : 0;
}
