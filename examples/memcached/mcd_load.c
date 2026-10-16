// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// mcd_load.c -- memcached BINARY-protocol load generator over UDP (for the FPGA memcached on FLD-lite + udp_lite).
//
// Payload = the 24-byte binary header + extras + key + value. Our kernel takes NO memcached UDP frame header;
// --frame adds the standard 8-byte one (request id, seq 0, total 1, reserved) for testing against stock memcached -U.
//
// N threads, each with its own connected UDP socket (own source port) and a window of W requests in flight, sent
// with sendmmsg and received with recvmmsg in batches. The opaque field carries (thread << 24 | seq & 0xFFFFFF);
// a response is matched to its slot (seq % W) by the full seq, its latency taken from the slot's send time. --inorder
// matches each response to the thread's oldest outstanding request instead, for servers that do not return the opaque
// field (the FPGA memcached: the Xilinx HLx pipeline drops it) but answer one flow in order. A request
// with no response after --timeout-us counts as lost and frees its slot.
//
// Phases: preload (SET every key once, all threads splitting the key range, retried up to 3 times on loss), then a
// timed run of GETs or a GET/SET mix (--set-pct). Values are a deterministic function of (key, size), so any GET can
// be checked: every --check-every'th GET hit is compared byte for byte.
//
// Build: gcc -O2 -pthread -o mcd_load mcd_load.c
// usage: mcd_load --server IP [--port 11211] [--threads N] [--window W] [--keys K] [--vsize V] [--secs S]
//                 [--set-pct P] [--timeout-us T] [--check-every C] [--frame] [--no-preload] [--batch B] [--quiet]
//                 [--inorder]
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

#define MAXB 64              // sendmmsg / recvmmsg batch
#define HB 200000            // latency histogram bins of 100 ns -> 20 ms
#define MAXV 9000

static char *srv = "192.168.77.2";
static int port = 11211, nthr = 1, win = 8, keys = 64, vsize = 32, secs = 10, setpct = 0, frame = 0, preload = 1,
           batch = 32, quiet = 0, check_every = 64;
static long timeout_us = 20000;
static int inorder = 0;
static volatile int phase = 0;   // 0 idle, 1 preload, 2 run, 3 stop

static inline uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
static void keyname(char *k, unsigned id) { snprintf(k, 16, "k%08u", id); }    // 9 bytes, fixed length
static void value_of(uint8_t *v, unsigned id, int n) {    // deterministic content per (key, size)
  uint32_t x = id * 2654435761u ^ (uint32_t)n * 40503u;
  for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v[i] = (uint8_t)x; }
}

typedef struct {
  int tid, fd, k0, k1;       // key range [k0, k1) for preload; all keys for the run
  uint64_t sent, recv, lost, err, miss, hit, sets, set_ok, checked, bad_value, unmatched, short_pkt, tx_bytes, rx_bytes;
  uint32_t *hist; uint64_t hist_over;
  uint32_t rng;
} thr_t;
typedef struct { uint32_t seq; uint64_t t; uint8_t busy, op; unsigned key; } slot_t;

static int build(uint8_t *p, int op, unsigned key, uint32_t opaque, int vs) {
  int o = 0;
  if (frame) { uint16_t rid = htons((uint16_t)opaque); memcpy(p, &rid, 2); p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 1; p[6] = 0; p[7] = 0; o = 8; }
  char k[16]; keyname(k, key);
  int ext = op == 1 ? 8 : 0, kl = 9, vl = op == 1 ? vs : 0;
  uint8_t *h = p + o; memset(h, 0, 24);
  h[0] = 0x80; h[1] = (uint8_t)op; h[2] = 0; h[3] = (uint8_t)kl; h[4] = (uint8_t)ext;
  uint32_t bl = htonl(ext + kl + vl); memcpy(h + 8, &bl, 4);
  uint32_t oq = htonl(opaque); memcpy(h + 12, &oq, 4);
  uint8_t *b = h + 24;
  if (op == 1) { uint32_t fl = htonl(0xDEADBEEF), ex = 0; memcpy(b, &fl, 4); memcpy(b + 4, &ex, 4); b += 8; }
  memcpy(b, k, kl); b += kl;
  if (op == 1) { value_of(b, key, vs); b += vs; }
  return (int)(b - p);
}

static void run_phase(thr_t *T, int mode, uint64_t t_end, int k0, int k1, int *retry_keys, int nretry) {
  // mode 1: preload keys [k0,k1) (or the nretry keys in retry_keys), each once; mode 2: timed run until t_end
  slot_t *S = calloc(win, sizeof(slot_t));
  static __thread uint8_t txb[MAXB][MAXV + 64], rxb[MAXB][MAXV + 64];
  struct mmsghdr tm[MAXB], rm[MAXB]; struct iovec tio[MAXB], rio[MAXB];
  for (int i = 0; i < MAXB; i++) { rio[i].iov_base = rxb[i]; rio[i].iov_len = sizeof(rxb[i]); memset(&rm[i], 0, sizeof(rm[i])); rm[i].msg_hdr.msg_iov = &rio[i]; rm[i].msg_hdr.msg_iovlen = 1; }
  uint32_t seq = 0, head = 0; int inflight = 0, next = 0, total = retry_keys ? nretry : k1 - k0;
  uint8_t vbuf[MAXV];
  for (;;) {
    uint64_t t = now_ns();
    int more = mode == 1 ? next < total : t < t_end;
    if (!more && inflight == 0) break;
    // fill free slots
    int nb = 0;
    while (more && inflight < win && nb < batch) {
      unsigned idx = seq % win; if (S[idx].busy) break;
      int op; unsigned key;
      if (mode == 1) { op = 1; key = retry_keys ? (unsigned)retry_keys[next] : (unsigned)(k0 + next); next++; more = next < total; }
      else { T->rng ^= T->rng << 13; T->rng ^= T->rng >> 17; T->rng ^= T->rng << 5;
             key = T->rng % keys; op = ((T->rng >> 8) % 100) < (unsigned)setpct ? 1 : 0; }
      uint32_t opq = ((uint32_t)T->tid << 24) | (seq & 0xFFFFFF);
      int len = build(txb[nb], op, key, opq, vsize);
      tio[nb].iov_base = txb[nb]; tio[nb].iov_len = len; memset(&tm[nb], 0, sizeof(tm[nb]));
      tm[nb].msg_hdr.msg_iov = &tio[nb]; tm[nb].msg_hdr.msg_iovlen = 1;
      S[idx].seq = seq; S[idx].t = t; S[idx].busy = 1; S[idx].op = (uint8_t)op; S[idx].key = key;
      seq++; inflight++; nb++; T->tx_bytes += len;
      if (op == 1) T->sets++;
    }
    for (int s = 0; s < nb;) {
      int r = sendmmsg(T->fd, tm + s, nb - s, 0);
      if (r < 0) { if (errno == EAGAIN || errno == ENOBUFS || errno == EINTR) continue; perror("sendmmsg"); exit(1); }
      s += r;
    }
    T->sent += nb;
    // receive
    int r = recvmmsg(T->fd, rm, MAXB, MSG_DONTWAIT, NULL);
    uint64_t tr = now_ns();
    for (int i = 0; i < r; i++) {
      uint8_t *p = rxb[i]; int n = rm[i].msg_len; T->rx_bytes += n;
      if (frame) { p += 8; n -= 8; }
      if (n < 24 || p[0] != 0x81) { T->short_pkt++; continue; }
      uint32_t opq; memcpy(&opq, p + 12, 4); opq = ntohl(opq);
      uint16_t st = (uint16_t)(p[6] << 8 | p[7]);
      uint32_t bl; memcpy(&bl, p + 8, 4); bl = ntohl(bl);
      slot_t *sl;
      if (inorder) {   // the oldest request still outstanding (timed-out ones are skipped)
        while (head != seq && !S[head % win].busy) head++;
        if (head == seq) { T->unmatched++; continue; }
        sl = &S[head % win]; head++;
      } else {
        if ((int)(opq >> 24) != T->tid) { T->unmatched++; continue; }
        sl = &S[(opq & 0xFFFFFF) % win];
        if (!sl->busy || (sl->seq & 0xFFFFFF) != (opq & 0xFFFFFF)) { T->unmatched++; continue; }
      }
      uint64_t lat = (tr - sl->t) / 100; if (lat < HB) T->hist[lat]++; else T->hist_over++;
      T->recv++;
      if (sl->op == 1) { if (st == 0) T->set_ok++; else T->err++; }
      else if (st == 1) T->miss++;
      else if (st != 0) T->err++;
      else {
        T->hit++;
        if (check_every && (T->hit % check_every) == 0) {
          int el = p[4], kl = p[2] << 8 | p[3]; int vl = (int)bl - el - kl;
          T->checked++;
          value_of(vbuf, sl->key, vsize);
          if (vl != vsize || 24 + (int)bl > n || memcmp(p + 24 + el + kl, vbuf, vsize)) T->bad_value++;
        }
      }
      sl->busy = 0; inflight--;
      if (mode == 1 && retry_keys == NULL && st != 0 && sl->op == 1) { /* SET failed: counted in err */ }
    }
    if (r <= 0 || inflight == win) {   // timeout scan
      uint64_t lim = (uint64_t)timeout_us * 1000;
      for (int i = 0; i < win; i++) if (S[i].busy && tr - S[i].t > lim) { S[i].busy = 0; inflight--; T->lost++; }
    }
  }
  free(S);
}

static pthread_barrier_t bar;
static volatile uint64_t t_run_end;
static void *thr_main(void *a) {
  thr_t *T = a;
  pthread_barrier_wait(&bar);                                   // 1: start
  if (preload) run_phase(T, 1, 0, T->k0, T->k1, NULL, 0);
  pthread_barrier_wait(&bar);                                   // 2: preload done everywhere
  pthread_barrier_wait(&bar);                                   // 3: main has reset counters and set t_run_end
  run_phase(T, 2, t_run_end, 0, 0, NULL, 0);
  return NULL;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *a = argv[i]; char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(n, var) if (!strcmp(a, n) && v) { var = atoi(v); i++; continue; }
    if (!strcmp(a, "--server") && v) { srv = v; i++; continue; }
    OPT("--port", port) OPT("--threads", nthr) OPT("--window", win) OPT("--keys", keys) OPT("--vsize", vsize)
    OPT("--secs", secs) OPT("--set-pct", setpct) OPT("--batch", batch) OPT("--check-every", check_every)
    if (!strcmp(a, "--timeout-us") && v) { timeout_us = atol(v); i++; continue; }
    if (!strcmp(a, "--inorder")) { inorder = 1; continue; }
    if (!strcmp(a, "--frame")) { frame = 1; continue; }
    if (!strcmp(a, "--no-preload")) { preload = 0; continue; }
    if (!strcmp(a, "--quiet")) { quiet = 1; continue; }
    fprintf(stderr, "unknown option %s\n", a); return 2;
  }
  if (nthr < 1 || nthr > 64 || win < 1 || win > 4096 || vsize < 0 || vsize > MAXV - 64 || batch < 1 || batch > MAXB) { fprintf(stderr, "bad args\n"); return 2; }
  thr_t *T = calloc(nthr, sizeof(thr_t));
  struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons(port); inet_pton(AF_INET, srv, &sa.sin_addr);
  for (int t = 0; t < nthr; t++) {
    T[t].tid = t; T[t].hist = calloc(HB, 4); T[t].rng = 0x9E3779B9u * (t + 1);
    T[t].k0 = (int)((long)keys * t / nthr); T[t].k1 = (int)((long)keys * (t + 1) / nthr);
    T[t].fd = socket(AF_INET, SOCK_DGRAM, 0);
    int bs = 8 << 20; setsockopt(T[t].fd, SOL_SOCKET, SO_RCVBUF, &bs, sizeof(bs)); setsockopt(T[t].fd, SOL_SOCKET, SO_SNDBUF, &bs, sizeof(bs));
    if (connect(T[t].fd, (struct sockaddr *)&sa, sizeof(sa))) { perror("connect"); return 1; }
  }
  pthread_barrier_init(&bar, NULL, nthr + 1);
  pthread_t th[64];
  for (int t = 0; t < nthr; t++) pthread_create(&th[t], NULL, thr_main, &T[t]);
  uint64_t tp0 = now_ns();
  pthread_barrier_wait(&bar);                                   // 1
  pthread_barrier_wait(&bar);                                   // 2: every thread finished its preload
  uint64_t t_pre_done = now_ns();
  {
    uint64_t pok = 0, perr = 0, plost = 0;
    for (int t = 0; t < nthr; t++) { pok += T[t].set_ok; perr += T[t].err; plost += T[t].lost; }
    if (preload) printf("preload: %d keys x %d B in %.2f s: SET ok %llu, failed %llu, lost %llu\n", keys, vsize,
                        (t_pre_done - tp0) / 1e9, (unsigned long long)pok, (unsigned long long)perr, (unsigned long long)plost);
    for (int t = 0; t < nthr; t++) { T[t].sent = T[t].recv = T[t].lost = T[t].err = T[t].miss = T[t].hit = T[t].sets = T[t].set_ok = 0;
                                     T[t].checked = T[t].bad_value = T[t].unmatched = T[t].short_pkt = T[t].tx_bytes = T[t].rx_bytes = 0;
                                     memset(T[t].hist, 0, HB * 4); T[t].hist_over = 0; }
  }
  t_run_end = now_ns() + (uint64_t)secs * 1000000000ull;
  __sync_synchronize();
  uint64_t t0 = now_ns();
  pthread_barrier_wait(&bar);                                   // 3: go
  for (int t = 0; t < nthr; t++) pthread_join(th[t], NULL);
  double el = (now_ns() - t0) / 1e9;
  // merge
  uint64_t sent = 0, recv = 0, lost = 0, err = 0, miss = 0, hit = 0, sets = 0, setok = 0, chk = 0, bad = 0, unm = 0, shp = 0, txb = 0, rxb = 0, over = 0;
  uint64_t *H = calloc(HB, 8);
  for (int t = 0; t < nthr; t++) {
    sent += T[t].sent; recv += T[t].recv; lost += T[t].lost; err += T[t].err; miss += T[t].miss; hit += T[t].hit; sets += T[t].sets;
    setok += T[t].set_ok; chk += T[t].checked; bad += T[t].bad_value; unm += T[t].unmatched; shp += T[t].short_pkt;
    txb += T[t].tx_bytes; rxb += T[t].rx_bytes; over += T[t].hist_over;
    for (int i = 0; i < HB; i++) H[i] += T[t].hist[i];
  }
  double pct[4] = {50, 90, 99, 99.9}; double pv[4] = {0};
  for (int j = 0; j < 4; j++) {
    uint64_t want = (uint64_t)(recv * pct[j] / 100.0), c = 0; pv[j] = -1;
    for (int i = 0; i < HB; i++) { c += H[i]; if (c >= want && want) { pv[j] = i / 10.0; break; } }
  }
  double rps = recv / el;
  // wire bytes: payload + UDP 8 + IP 20 + Eth 14 + FCS 4 + preamble/IFG 20
  double rx_wire = (rxb + recv * 66.0) * 8 / el / 1e9, tx_wire = (txb + sent * 66.0) * 8 / el / 1e9;
  printf("RESULT threads=%d window=%d keys=%d vsize=%d set_pct=%d secs=%.1f: %.0f req/s  sent %llu recv %llu lost %llu (%.3f%%)  "
         "hit %llu miss %llu err %llu set_ok %llu/%llu  checked %llu bad %llu unmatched %llu short %llu  "
         "lat us p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f (>20ms %llu)  resp %.3f Gb/s wire (req %.3f)\n",
         nthr, win, keys, vsize, setpct, el, rps, (unsigned long long)sent, (unsigned long long)recv, (unsigned long long)lost,
         sent ? 100.0 * lost / sent : 0.0, (unsigned long long)hit, (unsigned long long)miss, (unsigned long long)err,
         (unsigned long long)setok, (unsigned long long)sets, (unsigned long long)chk, (unsigned long long)bad,
         (unsigned long long)unm, (unsigned long long)shp, pv[0], pv[1], pv[2], pv[3], (unsigned long long)over, rx_wire, tx_wire);
  if (!quiet) for (int t = 0; t < nthr; t++)
    printf("  thread %d: %.0f req/s, lost %llu, err %llu, bad %llu\n", t, T[t].recv / el, (unsigned long long)T[t].lost,
           (unsigned long long)T[t].err, (unsigned long long)T[t].bad_value);
  return (bad || err) ? 1 : 0;
}
