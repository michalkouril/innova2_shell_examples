/*
 * sssp_ref.c -- CPU reference for L2/benchmarks/shortest_path_float_pred.
 * Reads the same offset / indices+weights files the host reads, picks the same source
 * (host main.cpp: first vertex with the strictly largest out-degree), and replays the
 * kernel's algorithm (L2/include/hw/shortest_path.hpp, pred namespace): a FIFO queue
 * of vertex IDs; pop u, for every out-edge compute curDist = dist[u] + w in float and
 * collect the strictly improving ones (curDist < dist[v]) into the table, then write
 * dist/pred for the table and push each table entry. pred[source] = source,
 * unreachable: dist = +inf, pred = 0xFFFFFFFF. Because it is the same order, pred
 * should match the kernel exactly (the host also accepts any equal-length path).
 * Also checks the kernel limits: queue overflow (wr-rd > qcfg-1, qcfg = 10*300*4096)
 * and table overflow (improving neighbours of one vertex > MAXOUTDEGREE = 40960).
 * Writes the golden in data-golden.sssp.mtx format: header line, then
 * "vertex+1,distance,pred+1" for every reachable vertex (1-based like the host expects).
 * Usage: sssp_ref <offsets> <indicesweights> <golden_out>
 * Build: gcc -O2 -o sssp_ref sssp_ref.c   (no -ffast-math: float adds must be IEEE)
 */
#include "csr_text.h"
#include <math.h>

#define QCFG (10u * 300u * 4096u)
#define MAXOUTDEGREE (4096 * 10)

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s offsets indicesweights golden_out\n", argv[0]); return 2; }
    csr_t g; memset(&g, 0, sizeof g);
    csr_read_offsets(argv[1], &g);
    csr_read_index(argv[2], &g, 1);
    uint32_t n = g.n;
    /* host source selection: max starts at 0, strict > */
    uint32_t src = 0, maxd = 0;
    for (uint32_t i = 0; i < n; i++) if (g.off[i + 1] - g.off[i] > maxd) { maxd = g.off[i + 1] - g.off[i]; src = i; }
    printf("n=%u nnz=%llu source(0-based)=%u max_out=%u\n", n, (unsigned long long)g.nnz, src, maxd);
    float* dist = malloc(sizeof(float) * n);
    uint32_t* pred = malloc(sizeof(uint32_t) * n);
    for (uint32_t i = 0; i < n; i++) { dist[i] = INFINITY; pred[i] = 0xFFFFFFFFu; }
    dist[src] = 0.0f; pred[src] = src;
    /* growable FIFO so an overflow is measured, not just flagged */
    size_t qcap = 1 << 24; uint32_t* q = malloc(sizeof(uint32_t) * qcap);
    uint64_t rd = 0, wr = 0, maxocc = 0, pops = 0, pushes = 0;
    q[wr++ % qcap] = src; /* kernel: ddrQue[0]=source, wr_cnt starts at 1 */
    uint32_t* tid = malloc(sizeof(uint32_t) * (maxd + 1));
    float* tdist = malloc(sizeof(float) * (maxd + 1));
    uint32_t maxtable = 0; int qover = 0, tover = 0;
    while (rd != wr) {
        uint32_t u = q[rd % qcap]; rd++; pops++;
        float du = dist[u];
        uint32_t cnt = 0;
        for (uint32_t e = g.off[u]; e < g.off[u + 1]; e++) {
            uint32_t v = g.col[e];
            float cd = du + g.w[e];
            if (cd < dist[v]) { tid[cnt] = v; tdist[cnt] = cd; cnt++; }
        }
        if (cnt > maxtable) maxtable = cnt;
        if (cnt > MAXOUTDEGREE) tover = 1;
        for (uint32_t i = 0; i < cnt; i++) {
            dist[tid[i]] = tdist[i]; pred[tid[i]] = u;
            if (wr - rd + 1 > qcap) { fprintf(stderr, "queue exceeds %zu, growing not implemented\n", qcap); return 1; }
            q[wr % qcap] = tid[i]; wr++; pushes++;
            if (wr - rd > QCFG - 1) qover = 1;
            if (wr - rd > maxocc) maxocc = wr - rd;
        }
    }
    uint64_t reach = 0; double sum = 0; float dmax = 0; uint64_t hsh = 1469598103934665603ULL;
    uint64_t hist[64] = {0};
    FILE* fo = fopen(argv[3], "w"); if (!fo) { perror(argv[3]); return 1; }
    fprintf(fo, "Vertex_ID,Distance,Pred\n");
    for (uint32_t v = 0; v < n; v++) {
        if (dist[v] == INFINITY) continue;
        reach++; sum += dist[v]; if (dist[v] > dmax) dmax = dist[v];
        if (dist[v] < 63 && dist[v] == floorf(dist[v])) hist[(int)dist[v]]++;
        fprintf(fo, "%u,%.9g,%u\n", v + 1, dist[v], pred[v] + 1);
        uint32_t bits; memcpy(&bits, &dist[v], 4);
        hsh = (hsh ^ v) * 1099511628211ULL; hsh = (hsh ^ bits) * 1099511628211ULL;
    }
    if (fclose(fo)) { perror("close"); return 1; }
    printf("reachable=%llu (incl. source) sum_dist=%.1f max_dist=%g fnv64(v,distbits)=%016llx\n",
           (unsigned long long)reach, sum, dmax, (unsigned long long)hsh);
    printf("pops=%llu pushes=%llu max_queue_occupancy=%llu (limit qcfg-1=%u) queue_overflow=%d\n",
           (unsigned long long)pops, (unsigned long long)pushes, (unsigned long long)maxocc, QCFG - 1, qover);
    printf("max_table=%u (limit MAXOUTDEGREE=%d) table_overflow=%d\n", maxtable, MAXOUTDEGREE, tover);
    printf("hop histogram (integer distances):");
    for (int i = 0; i < 64; i++) if (hist[i]) printf(" %d:%llu", i, (unsigned long long)hist[i]);
    printf("\n");
    return 0;
}
