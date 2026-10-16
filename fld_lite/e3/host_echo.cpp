// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Baseline for the E3 echo latency breakdown (2026-09-30): the SAME ping-pong, but the CPU does the echo on port 0
// (verbs raw QPs, host memory, busy polling, zero copy: the received buffer is re-sent with the header rewritten).
// All four completion queues carry ConnectX hardware timestamps, so "CPU echo: port0 RX CQE -> TX CQE" is directly
// comparable with e3_echo_host's "FPGA echo" line.
//   usage (root): host_echo <port0 dev> <port1 dev> [frames] [len 64..1024]
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const size_t BUFLEN = 1024;
static size_t FRAME = 1024;   // [frames] [len]
static const uint16_t ET_IN = 0x88B6, ET_OUT = 0x88B8;

static ibv_context *open_dev(const char *name)
{
  int n = 0; ibv_device **l = ibv_get_device_list(&n); ibv_context *c = nullptr;
  for (int i = 0; i < n; i++) if (!strcmp(ibv_get_device_name(l[i]), name)) c = ibv_open_device(l[i]);
  ibv_free_device_list(l); return c;
}
static ibv_cq_ex *cq_ts(ibv_context *c, int n)
{
  ibv_cq_init_attr_ex a{}; a.cqe = n; a.wc_flags = IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
  ibv_cq_ex *q = ibv_create_cq_ex(c, &a); if (!q) { perror("create_cq_ex"); exit(1); } return q;
}
template <class F> static int poll_ts(ibv_cq_ex *q, F f)
{
  ibv_poll_cq_attr pa{}; if (ibv_start_poll(q, &pa)) return 0;
  int k = 0;
  do { f(q->wr_id, q->status, ibv_wc_read_byte_len(q), ibv_wc_read_completion_ts(q)); k++; } while (!ibv_next_poll(q));
  ibv_end_poll(q); return k;
}
static ibv_qp *raw_qp(ibv_pd *pd, ibv_cq *cq, int sw, int rw)
{
  ibv_qp_init_attr ia{}; ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = sw; ia.cap.max_recv_wr = rw; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia); if (!qp) { perror("create_qp"); exit(1); }
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  a = {}; a.qp_state = IBV_QPS_RTS; ibv_modify_qp(qp, &a, IBV_QP_STATE);
  return qp;
}
static ibv_flow *steer(ibv_qp *qp, uint16_t et)
{
  struct { ibv_flow_attr a; ibv_flow_spec_eth e; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 1; fl.a.port = 1;
  fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(et); fl.e.mask.ether_type = 0xFFFF;
  ibv_flow *f = ibv_create_flow(qp, &fl.a); if (!f) { perror("create_flow"); exit(1); } return f;
}
static void make_frame(uint8_t *f, unsigned i)
{
  memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x04", 6); f[12] = ET_IN >> 8; f[13] = ET_IN & 0xFF;
  for (size_t k = 14; k < FRAME; k++) f[k] = (uint8_t)(k * 13 + i * 5);
  f[14] = 0xEE; f[15] = i & 0xFF; f[16] = (i >> 8) & 0xFF;
}
struct St { double m, p50, p99; };
static St stats(std::vector<double> v) { std::sort(v.begin(), v.end()); double m = 0; for (double x : v) m += x; m /= v.size(); return {m, v[v.size() / 2], v[v.size() * 99 / 100]}; }

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: %s <port0 dev> <port1 dev> [frames]\n", argv[0]); return 2; }
  unsigned n = argc > 3 ? atoi(argv[3]) : 1000;
  if (argc > 4) FRAME = atoi(argv[4]);
  ibv_context *c0 = open_dev(argv[1]), *c1 = open_dev(argv[2]);
  ibv_pd *p0 = ibv_alloc_pd(c0), *p1 = ibv_alloc_pd(c1);
  uint8_t *b0 = (uint8_t *)aligned_alloc(4096, 256 * BUFLEN), *tx = (uint8_t *)aligned_alloc(4096, 64 * BUFLEN), *b1 = (uint8_t *)aligned_alloc(4096, 256 * BUFLEN);
  ibv_mr *m0 = ibv_reg_mr(p0, b0, 256 * BUFLEN, IBV_ACCESS_LOCAL_WRITE), *mt = ibv_reg_mr(p1, tx, 64 * BUFLEN, 0), *m1 = ibv_reg_mr(p1, b1, 256 * BUFLEN, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq_ex *q0r = cq_ts(c0, 512), *q0t = cq_ts(c0, 512), *q1t = cq_ts(c1, 128), *q1r = cq_ts(c1, 512);
  ibv_qp *e0r = raw_qp(p0, ibv_cq_ex_to_cq(q0r), 1, 256), *e0t = raw_qp(p0, ibv_cq_ex_to_cq(q0t), 256, 1);
  ibv_qp *s1 = raw_qp(p1, ibv_cq_ex_to_cq(q1t), 64, 1), *r1 = raw_qp(p1, ibv_cq_ex_to_cq(q1r), 1, 256);
  ibv_flow *f0 = steer(e0r, ET_IN), *f1 = steer(r1, ET_OUT);
  auto post_rx = [](ibv_qp *qp, uint8_t *b, ibv_mr *mr, unsigned slot) {
    ibv_sge s{(uint64_t)(b + slot * BUFLEN), (uint32_t)BUFLEN, mr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = slot; w.sg_list = &s; w.num_sge = 1;
    ibv_post_recv(qp, &w, &bad);
  };
  for (unsigned i = 0; i < 256; i++) { post_rx(e0r, b0, m0, i); post_rx(r1, b1, m1, i); }

  std::vector<double> rtt, host, wo, cpu, wb;
  unsigned good = 0;
  for (unsigned i = 0; i < n; i++) {
    uint64_t t1tx = 0, t0rx = 0, t0tx = 0, t1rx = 0;
    uint8_t *f = tx + (i % 64) * BUFLEN; make_frame(f, i);
    auto ts = std::chrono::steady_clock::now();
    ibv_sge sg{(uint64_t)f, (uint32_t)FRAME, mt->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = i; w.sg_list = &sg; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    ibv_post_send(s1, &w, &bad);
    bool echoed = false, back = false;
    while (!back && std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count() < 0.2) {
      poll_ts(q1t, [&](uint64_t, ibv_wc_status, uint32_t, uint64_t t) { t1tx = t; });
      if (!echoed) poll_ts(q0r, [&](uint64_t wr, ibv_wc_status st, uint32_t len, uint64_t t) {   // the CPU echo
        t0rx = t; uint8_t *p = b0 + wr * BUFLEN; p[6] = 0x02; p[11] = 0xEC; p[12] = ET_OUT >> 8; p[13] = ET_OUT & 0xFF;
        ibv_sge s{(uint64_t)p, len, m0->lkey}; ibv_send_wr ew{}, *eb; ew.wr_id = wr; ew.sg_list = &s; ew.num_sge = 1;
        ew.opcode = IBV_WR_SEND; ew.send_flags = IBV_SEND_SIGNALED; ibv_post_send(e0t, &ew, &eb); echoed = true; });
      poll_ts(q0t, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { t0tx = t; post_rx(e0r, b0, m0, (unsigned)wr); });
      poll_ts(q1r, [&](uint64_t wr, ibv_wc_status st, uint32_t len, uint64_t t) {
        t1rx = t; uint8_t ref[1024]; make_frame(ref, i); ref[6] = 0x02; ref[11] = 0xEC; ref[12] = ET_OUT >> 8; ref[13] = ET_OUT & 0xFF;
        good += !st && len == FRAME && !memcmp(b1 + wr * BUFLEN, ref, FRAME); post_rx(r1, b1, m1, (unsigned)wr); back = true; });
    }
    if (!back) continue;
    double r = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - ts).count(); rtt.push_back(r);
    for (int k = 0; k < 1000 && !t0tx; k++) poll_ts(q0t, [&](uint64_t wr, ibv_wc_status, uint32_t, uint64_t t) { t0tx = t; post_rx(e0r, b0, m0, (unsigned)wr); });
    for (int k = 0; k < 1000 && !t1tx; k++) poll_ts(q1t, [&](uint64_t, ibv_wc_status, uint32_t, uint64_t t) { t1tx = t; });
    if (t1tx && t0rx && t0tx && t1rx && t1tx <= t0rx && t0rx <= t0tx && t0tx <= t1rx) {
      const double us = 1.0 / 156.25;
      wo.push_back((t0rx - t1tx) * us); cpu.push_back((t0tx - t0rx) * us); wb.push_back((t1rx - t0tx) * us); host.push_back(r - (t1rx - t1tx) * us);
    }
  }
  auto pr = [](const char *nm, const std::vector<double> &v) { if (v.empty()) { printf("  %-38s (no samples)\n", nm); return; } St s = stats(v); printf("  %-38s mean %6.2f us  p50 %6.2f  p99 %6.2f\n", nm, s.m, s.p50, s.p99); };
  St s = stats(rtt);
  printf("round trip (host send -> CPU echo -> host receive), %zu samples: mean %.2f us, p50 %.2f, p99 %.2f; byte-exact %u/%u\n", rtt.size(), s.m, s.p50, s.p99, good, n);
  printf("breakdown (%zu frames with all four ConnectX timestamps):\n", cpu.size());
  pr("host: post_send..TX + RX CQE..seen", host); pr("wire out: port1 TX -> port0 RX", wo);
  pr("CPU echo: port0 RX CQE -> TX CQE", cpu); pr("wire back: port0 TX -> port1 RX", wb);
  ibv_destroy_flow(f0); ibv_destroy_flow(f1);
  return 0;
}
