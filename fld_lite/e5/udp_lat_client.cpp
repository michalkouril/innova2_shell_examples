// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// udp_lat_client -- UDP step 1/2 latency: ping-pong UDP datagrams to the FPGA endpoint through a raw-packet QP (no
// kernel stack on the client), round trip from the client NIC's own completion timestamps (TX CQE -> RX CQE, one
// clock), plus the host-observed time. One datagram in flight. Checksums computed here (no offload on the client).
//   usage (root): udp_lat_client <dev> <netdev> <dst mac aa:bb:..> [count] [payload sizes...]
//     e.g. udp_lat_client mlx5_1 enp4s0f1 00:00:00:00:00:01 2000 18 64 256 1024 1472
// Addresses: 192.168.77.1:40001 -> 192.168.77.2:7777 (the e5udp_host defaults).
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint32_t SRC_IP = 0xC0A84D01, DST_IP = 0xC0A84D02;
static const uint16_t SPORT = 40001, DPORT = 7777;
static const unsigned SLOT = 2048, NRX = 64;

static ibv_context *open_dev(const char *name)
{
  int n = 0; ibv_device **l = ibv_get_device_list(&n); ibv_context *c = nullptr;
  for (int i = 0; i < n; i++) if (!strcmp(ibv_get_device_name(l[i]), name)) c = ibv_open_device(l[i]);
  ibv_free_device_list(l); return c;
}
static uint16_t csum16(const uint8_t *p, size_t n, uint32_t s = 0)
{
  for (size_t i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
  if (n & 1) s += p[n - 1] << 8;
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return (uint16_t)~s;
}
static size_t build(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, unsigned plen, unsigned seq)
{
  const size_t ulen = 8 + plen;
  memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 0x08; f[13] = 0x00;
  uint8_t *ip = f + 14; memset(ip, 0, 20);
  ip[0] = 0x45; ip[2] = (20 + ulen) >> 8; ip[3] = (20 + ulen) & 0xff; ip[4] = seq >> 8; ip[5] = seq; ip[6] = 0x40; ip[8] = 64; ip[9] = 17;
  ip[12] = SRC_IP >> 24; ip[13] = SRC_IP >> 16; ip[14] = SRC_IP >> 8; ip[15] = SRC_IP;
  ip[16] = DST_IP >> 24; ip[17] = DST_IP >> 16; ip[18] = DST_IP >> 8; ip[19] = DST_IP;
  uint16_t c = csum16(ip, 20); ip[10] = c >> 8; ip[11] = c;
  uint8_t *u = ip + 20;
  u[0] = SPORT >> 8; u[1] = SPORT & 0xff; u[2] = DPORT >> 8; u[3] = DPORT & 0xff; u[4] = ulen >> 8; u[5] = ulen; u[6] = u[7] = 0;
  for (unsigned i = 0; i < plen; i++) u[8 + i] = (uint8_t)(i * 3 + seq);
  uint32_t s = 0; for (int i = 12; i < 20; i += 2) s += (ip[i] << 8) | ip[i + 1];
  c = csum16(u, ulen, s + 17 + ulen); if (!c) c = 0xffff; u[6] = c >> 8; u[7] = c;
  size_t len = 14 + 20 + ulen; if (len < 60) { memset(f + len, 0, 60 - len); len = 60; }
  return len;
}
static ibv_cq_ex *cq_ts(ibv_context *c, int n)
{
  ibv_cq_init_attr_ex a{}; a.cqe = n; a.wc_flags = IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
  ibv_cq_ex *q = ibv_create_cq_ex(c, &a); if (!q) { perror("create_cq_ex"); exit(1); } return q;
}
struct comp { uint64_t wr; int status; uint32_t len; uint64_t ts; };
static bool poll1(ibv_cq_ex *q, comp &o)
{
  ibv_poll_cq_attr pa{}; if (ibv_start_poll(q, &pa)) return false;
  o.wr = q->wr_id; o.status = q->status; o.len = ibv_wc_read_byte_len(q); o.ts = ibv_wc_read_completion_ts(q);
  ibv_end_poll(q); return true;
}
static void stats(std::vector<double> v, const char *name)
{
  if (v.empty()) { printf("  %-34s (no samples)\n", name); return; }
  std::sort(v.begin(), v.end()); double m = 0; for (double x : v) m += x; m /= v.size();
  printf("  %-34s mean %7.3f us  p50 %7.3f  p99 %7.3f  min %7.3f\n", name, m, v[v.size() / 2], v[v.size() * 99 / 100], v[0]);
}

int main(int argc, char **argv)
{
  if (argc < 4) { fprintf(stderr, "usage: %s <dev> <netdev> <dst mac> [count] [payload sizes...]\n", argv[0]); return 2; }
  const unsigned n = argc > 4 ? atoi(argv[4]) : 2000;
  std::vector<unsigned> sizes; for (int i = 5; i < argc; i++) sizes.push_back(atoi(argv[i]));
  if (sizes.empty()) sizes = {18, 64, 256, 1024, 1472};
  uint8_t dmac[6], smac[6]; unsigned v[6];
  if (sscanf(argv[3], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { fprintf(stderr, "bad mac\n"); return 2; }
  for (int i = 0; i < 6; i++) dmac[i] = v[i];
  { FILE *f = fopen((std::string("/sys/class/net/") + argv[2] + "/address").c_str(), "r");
    if (!f || fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { fprintf(stderr, "mac of %s?\n", argv[2]); return 1; }
    fclose(f); for (int i = 0; i < 6; i++) smac[i] = v[i]; }
  ibv_context *c = open_dev(argv[1]);
  if (!c) {
    int nd = 0; ibv_device **l = ibv_get_device_list(&nd);
    fprintf(stderr, "cannot open %s (errno %d); %d devices visible:", argv[1], errno, nd);
    for (int i = 0; i < nd; i++) fprintf(stderr, " %s", ibv_get_device_name(l[i]));
    fprintf(stderr, "\n"); return 1;
  }
  ibv_device_attr_ex da{}; ibv_query_device_ex(c, nullptr, &da);
  const double us_per_tick = 1000.0 / (double)da.hca_core_clock;
  ibv_pd *pd = ibv_alloc_pd(c);
  ibv_cq_ex *scq = cq_ts(c, 256), *rcq = cq_ts(c, 256);
  ibv_qp_init_attr ia{}; ia.send_cq = ibv_cq_ex_to_cq(scq); ia.recv_cq = ibv_cq_ex_to_cq(rcq); ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = 64; ia.cap.max_recv_wr = NRX; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia); if (!qp) { perror("create_qp"); return 1; }
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  a = {}; a.qp_state = IBV_QPS_RTS; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  uint8_t *tx = (uint8_t *)aligned_alloc(4096, SLOT), *rx = (uint8_t *)aligned_alloc(4096, NRX * SLOT);
  ibv_mr *tmr = ibv_reg_mr(pd, tx, SLOT, 0), *rmr = ibv_reg_mr(pd, rx, NRX * SLOT, IBV_ACCESS_LOCAL_WRITE);
  struct { ibv_flow_attr a; ibv_flow_spec_ipv4 i; ibv_flow_spec_tcp_udp u; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 2; fl.a.port = 1;
  fl.i.type = IBV_FLOW_SPEC_IPV4; fl.i.size = sizeof(fl.i); fl.i.val.dst_ip = htonl(SRC_IP); fl.i.mask.dst_ip = 0xffffffff;
  fl.u.type = IBV_FLOW_SPEC_UDP; fl.u.size = sizeof(fl.u); fl.u.val.dst_port = htons(SPORT); fl.u.mask.dst_port = 0xffff;
  ibv_flow *flow = ibv_create_flow(qp, &fl.a); if (!flow) { perror("create_flow"); return 1; }
  auto post_rx = [&](unsigned s) {
    ibv_sge g{(uint64_t)(uintptr_t)(rx + s * SLOT), SLOT, rmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = s; w.sg_list = &g; w.num_sge = 1;
    ibv_post_recv(qp, &w, &bad);
  };
  for (unsigned s = 0; s < NRX; s++) post_rx(s);
  printf("udp_lat_client: %s (%s) -> FPGA 192.168.77.2:%u via %02x:%02x:%02x:%02x:%02x:%02x, %u ping-pongs per size, NIC clock %llu kHz\n",
         argv[1], argv[2], DPORT, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5], n, (unsigned long long)da.hca_core_clock);
  int bad_total = 0;
  for (unsigned plen : sizes) {
    std::vector<double> nic, host; unsigned lost = 0, bad = 0;
    for (unsigned i = 0; i < n + 20; i++) {                  // the first 20 are warm-up (not counted)
      const size_t len = build(tx, dmac, smac, plen, i);
      ibv_sge g{(uint64_t)(uintptr_t)tx, (uint32_t)len, tmr->lkey}; ibv_send_wr w{}, *bw;
      w.wr_id = i; w.sg_list = &g; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
      auto t0 = std::chrono::steady_clock::now();
      if (ibv_post_send(qp, &w, &bw)) { perror("post_send"); return 1; }
      comp s{}, r{}; bool gs = false, gr = false, ok = false;
      while (!(gs && gr) && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.1) {
        if (!gs && poll1(scq, s)) gs = s.wr == i;                 // a late completion of an earlier send is skipped
        if (!gr && poll1(rcq, r)) {
          const uint8_t *f = rx + r.wr * SLOT;
          const bool mine = r.status == IBV_WC_SUCCESS && r.len >= 42 + plen && !memcmp(f + 42, tx + 42, plen);
          if (mine) { gr = true; ok = f[36] == (SPORT >> 8) && f[37] == (SPORT & 0xff) && !memcmp(f, smac, 6); }
          post_rx(r.wr);                                          // a stale echo of an earlier ping is dropped
        }
      }
      const double h = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      if (!gr) { lost++; continue; }
      if (!ok) bad++;
      if (i >= 20 && ok && gs && r.ts > s.ts) { nic.push_back((r.ts - s.ts) * us_per_tick); host.push_back(h); }
    }
    printf("payload %4u B (frame %4u B): %zu samples, lost %u, wrong %u\n", plen, (unsigned)std::max<size_t>(60, 42 + plen), nic.size(), lost, bad);
    stats(nic, "round trip, client NIC TX->RX");
    stats(host, "round trip, client host");
    bad_total += bad + (lost > n / 100 ? 1 : 0);
  }
  printf("UDP-LAT %s\n", bad_total ? "PROBLEM" : "OK");
  ibv_destroy_flow(flow); ibv_destroy_qp(qp);
  return bad_total ? 1 : 0;
}
