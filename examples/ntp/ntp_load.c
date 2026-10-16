// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// ntp_load.c -- NTP client load generator over UDP (for the FPGA NTP server on FLD-lite), in the style of
// examples/memcached/mcd_load.c: N threads, each with its own connected UDP socket and a window of W requests in
// flight, sent with sendmmsg and received with recvmmsg in batches. Each request is a 48-byte NTPv4 mode-3 packet whose
// transmit timestamp carries (thread << 48 | seq); a reply is matched by its origin timestamp, so every reply is
// checked: mode 4, version 4, origin = a request in flight, receive <= transmit. A request with no reply after
// --timeout-us counts as lost and frees its slot. Latency (send -> receive, CLOCK_MONOTONIC) as percentiles.
//
// Build: gcc -O2 -pthread -o ntp_load ntp_load.c
// usage: ntp_load --server IP [--port 123] [--threads N] [--window W] [--secs S] [--timeout-us T] [--batch B] [--quiet]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXB 64
#define HB 200000            // latency histogram bins of 100 ns -> 20 ms

static char *srv = "192.168.77.2";
static int port = 123, nthr = 1, win = 8, secs = 5, batch = 32, quiet = 0;
static long timeout_us = 20000;

static inline uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
static inline uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static inline void pbe64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }

typedef struct {
  int tid, fd;
  uint64_t sent, recv, lost, bad, unmatched;
  uint32_t *hist; uint64_t hist_over;
} thr_t;
typedef struct { uint64_t seq, t; uint8_t busy; } slot_t;

static volatile uint64_t t_end;
static pthread_barrier_t bar;

static void *thr_main(void *a) {
  thr_t *T = a;
  slot_t *S = calloc(win, sizeof(slot_t));
  static __thread uint8_t txb[MAXB][48], rxb[MAXB][256];
  struct mmsghdr tm[MAXB], rm[MAXB]; struct iovec tio[MAXB], rio[MAXB];
  for (int i = 0; i < MAXB; i++) { rio[i].iov_base = rxb[i]; rio[i].iov_len = sizeof(rxb[i]); memset(&rm[i], 0, sizeof(rm[i])); rm[i].msg_hdr.msg_iov = &rio[i]; rm[i].msg_hdr.msg_iovlen = 1; }
  uint64_t seq = 0; int inflight = 0;
  pthread_barrier_wait(&bar);
  for (;;) {
    uint64_t t = now_ns();
    int more = t < t_end;
    if (!more && inflight == 0) break;
    int nb = 0;
    while (more && inflight < win && nb < batch) {
      unsigned idx = seq % win; if (S[idx].busy) break;
      uint8_t *p = txb[nb]; memset(p, 0, 48);
      p[0] = (0 << 6) | (4 << 3) | 3; p[2] = 6; p[3] = 0xec;
      pbe64(p + 40, (uint64_t)T->tid << 48 | (seq & 0xffffffffffffull));
      tio[nb].iov_base = p; tio[nb].iov_len = 48; memset(&tm[nb], 0, sizeof(tm[nb]));
      tm[nb].msg_hdr.msg_iov = &tio[nb]; tm[nb].msg_hdr.msg_iovlen = 1;
      S[idx].seq = seq; S[idx].t = t; S[idx].busy = 1;
      seq++; inflight++; nb++;
    }
    for (int s = 0; s < nb;) {
      int r = sendmmsg(T->fd, tm + s, nb - s, 0);
      if (r < 0) { if (errno == EAGAIN || errno == ENOBUFS || errno == EINTR) continue; perror("sendmmsg"); exit(1); }
      s += r;
    }
    T->sent += nb;
    int r = recvmmsg(T->fd, rm, MAXB, MSG_DONTWAIT, NULL);
    uint64_t tr = now_ns();
    for (int i = 0; i < r; i++) {
      uint8_t *p = rxb[i]; int n = rm[i].msg_len;
      if (n < 48 || (p[0] & 7) != 4 || ((p[0] >> 3) & 7) != 4) { T->bad++; continue; }
      uint64_t org = be64(p + 24), rec = be64(p + 32), xmt = be64(p + 40);
      if ((int)(org >> 48) != T->tid) { T->unmatched++; continue; }
      slot_t *sl = &S[(org & 0xffffffffffffull) % win];
      if (!sl->busy || (sl->seq & 0xffffffffffffull) != (org & 0xffffffffffffull)) { T->unmatched++; continue; }
      if (rec > xmt) T->bad++;
      uint64_t lat = (tr - sl->t) / 100; if (lat < HB) T->hist[lat]++; else T->hist_over++;
      T->recv++; sl->busy = 0; inflight--;
    }
    if (r <= 0 || inflight == win) {
      uint64_t lim = (uint64_t)timeout_us * 1000;
      for (int i = 0; i < win; i++) if (S[i].busy && tr - S[i].t > lim) { S[i].busy = 0; inflight--; T->lost++; }
    }
  }
  free(S);
  return NULL;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *a = argv[i]; char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(n, var) if (!strcmp(a, n) && v) { var = atoi(v); i++; continue; }
    if (!strcmp(a, "--server") && v) { srv = v; i++; continue; }
    OPT("--port", port) OPT("--threads", nthr) OPT("--window", win) OPT("--secs", secs) OPT("--batch", batch)
    if (!strcmp(a, "--timeout-us") && v) { timeout_us = atol(v); i++; continue; }
    if (!strcmp(a, "--quiet")) { quiet = 1; continue; }
    fprintf(stderr, "unknown option %s\n", a); return 2;
  }
  if (nthr < 1 || nthr > 64 || win < 1 || win > 4096 || batch < 1 || batch > MAXB) { fprintf(stderr, "bad args\n"); return 2; }
  thr_t *T = calloc(nthr, sizeof(thr_t));
  struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons(port); inet_pton(AF_INET, srv, &sa.sin_addr);
  for (int t = 0; t < nthr; t++) {
    T[t].tid = t; T[t].hist = calloc(HB, 4);
    T[t].fd = socket(AF_INET, SOCK_DGRAM, 0);
    int bs = 8 << 20; setsockopt(T[t].fd, SOL_SOCKET, SO_RCVBUF, &bs, sizeof(bs)); setsockopt(T[t].fd, SOL_SOCKET, SO_SNDBUF, &bs, sizeof(bs));
    if (connect(T[t].fd, (struct sockaddr *)&sa, sizeof(sa))) { perror("connect"); return 1; }
  }
  pthread_barrier_init(&bar, NULL, nthr + 1);
  pthread_t th[64];
  for (int t = 0; t < nthr; t++) pthread_create(&th[t], NULL, thr_main, &T[t]);
  t_end = now_ns() + (uint64_t)secs * 1000000000ull;
  __sync_synchronize();
  uint64_t t0 = now_ns();
  pthread_barrier_wait(&bar);
  for (int t = 0; t < nthr; t++) pthread_join(th[t], NULL);
  double el = (now_ns() - t0) / 1e9;
  uint64_t sent = 0, recv = 0, lost = 0, bad = 0, unm = 0, over = 0;
  uint64_t *H = calloc(HB, 8);
  for (int t = 0; t < nthr; t++) {
    sent += T[t].sent; recv += T[t].recv; lost += T[t].lost; bad += T[t].bad; unm += T[t].unmatched; over += T[t].hist_over;
    for (int i = 0; i < HB; i++) H[i] += T[t].hist[i];
  }
  double pct[4] = {50, 90, 99, 99.9}, pv[4] = {0};
  for (int j = 0; j < 4; j++) {
    uint64_t want = (uint64_t)(recv * pct[j] / 100.0), c = 0; pv[j] = -1;
    for (int i = 0; i < HB; i++) { c += H[i]; if (c >= want && want) { pv[j] = i / 10.0; break; } }
  }
  printf("RESULT threads=%d window=%d secs=%.1f: %.0f replies/s  sent %llu recv %llu lost %llu (%.4f%%)  bad %llu unmatched %llu  "
         "lat us p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f (>20ms %llu)\n",
         nthr, win, el, recv / el, (unsigned long long)sent, (unsigned long long)recv, (unsigned long long)lost,
         sent ? 100.0 * lost / sent : 0.0, (unsigned long long)bad, (unsigned long long)unm, pv[0], pv[1], pv[2], pv[3],
         (unsigned long long)over);
  if (!quiet) for (int t = 0; t < nthr; t++)
    printf("  thread %d: %.0f replies/s, lost %llu, bad %llu\n", t, T[t].recv / el, (unsigned long long)T[t].lost, (unsigned long long)T[t].bad);
  return bad ? 1 : 0;
}
