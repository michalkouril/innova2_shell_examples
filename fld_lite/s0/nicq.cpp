// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// nicq -- UDP architecture step 0 (work/docs/udp_stack_architecture.md, section 8): the five ConnectX-5 questions,
// host only, no FPGA. Two ports joined by a cable: A = the port FLD-lite would own, B = the "outside world".
//   A. RX: a DevX RQ + direct TIR on port A behind a NIC_RX rule {IPv4, UDP, dst ip, dst port, frag 0} + TAG; frames
//      crafted on port B (good / bad IP csum / bad UDP csum / UDP csum 0 / IP options / other port / fragment); the raw
//      64-byte CQEs are decoded at the offsets FLD-lite would use.
//   B. TX: a DevX SQ on port A sends UDP frames with BOTH checksum fields zeroed: 42-byte inline header + pointer
//      payload with cs_flags L3|L4, fully inline with cs_flags, and a control without cs_flags; port B receives and
//      checks the checksums in software.
//   usage (root): nicq <dev A> <netdev A> <dev B> <netdev B>      e.g. nicq mlx5_1 enp4s0f1 mlx5_3 enp9s0f1
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "devx_rx_s0.h"
#include "devx_sq.h"

static const uint32_t IP_A = 0xC0A84D02, IP_B = 0xC0A84D01;   // 192.168.77.2 (the "FPGA"), 192.168.77.1
static const uint16_t PORT_A = 7777, PORT_TX = 7778, TAG = 0x00ABCD;
static int fails = 0;
static void check(bool ok, const std::string &w) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", w.c_str()); fails += !ok; }

static ibv_context *open_dev(const char *name, bool devx)
{
  int n = 0; ibv_device **l = ibv_get_device_list(&n); ibv_context *c = nullptr;
  for (int i = 0; i < n; i++)
    if (!strcmp(ibv_get_device_name(l[i]), name)) {
      if (devx) { mlx5dv_context_attr a{}; a.flags = MLX5DV_CONTEXT_FLAGS_DEVX; c = mlx5dv_open_device(l[i], &a); }
      else c = ibv_open_device(l[i]);
    }
  ibv_free_device_list(l); return c;
}
static void mac_of(const char *nd, uint8_t m[6])
{
  FILE *f = fopen((std::string("/sys/class/net/") + nd + "/address").c_str(), "r"); unsigned v[6] = {0};
  if (!f || fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { fprintf(stderr, "MAC of %s?\n", nd); exit(1); }
  fclose(f); for (int i = 0; i < 6; i++) m[i] = v[i];
}
static uint16_t csum16(const uint8_t *p, size_t n, uint32_t s = 0)
{
  for (size_t i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
  if (n & 1) s += p[n - 1] << 8;
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return (uint16_t)~s;
}
static uint16_t udp_csum(const uint8_t *ip, const uint8_t *udp, size_t ulen)
{
  uint32_t s = 0;
  for (int i = 12; i < 20; i += 2) s += (ip[i] << 8) | ip[i + 1];        // src + dst ip
  s += 17 + ulen;
  uint16_t c = csum16(udp, ulen, s); return c ? c : 0xffff;
}
// a received UDP checksum is valid when the one's-complement sum over pseudo header + UDP datagram is 0xffff
static bool udp_valid(const uint8_t *ip, const uint8_t *u)
{
  const size_t ulen = (u[4] << 8) | u[5]; uint32_t s = 0;
  for (int i = 12; i < 20; i += 2) s += (ip[i] << 8) | ip[i + 1];
  return csum16(u, ulen, s + 17 + ulen) == 0;
}
// Ethernet + IPv4 (+ optional 4-byte option) + UDP; returns the frame length
enum { F_BAD_IP = 1, F_BAD_UDP = 2, F_ZERO_UDP = 4, F_OPT = 8, F_FRAG = 16, F_ZERO_ALL = 32 };
static size_t build(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t sip, uint32_t dip, uint16_t sport,
                    uint16_t dport, size_t plen, int fl, uint8_t seed)
{
  const size_t ihl = fl & F_OPT ? 24 : 20, ulen = 8 + plen;
  memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 0x08; f[13] = 0x00;
  uint8_t *ip = f + 14; memset(ip, 0, ihl);
  ip[0] = 0x40 | (ihl / 4); ip[2] = (ihl + ulen) >> 8; ip[3] = (ihl + ulen) & 0xff; ip[4] = 0x12; ip[5] = seed;
  ip[6] = fl & F_FRAG ? 0x20 : 0x40;                                   // MF (fragment) or DF
  ip[8] = 64; ip[9] = 17;
  ip[12] = sip >> 24; ip[13] = sip >> 16; ip[14] = sip >> 8; ip[15] = sip;
  ip[16] = dip >> 24; ip[17] = dip >> 16; ip[18] = dip >> 8; ip[19] = dip;
  if (fl & F_OPT) ip[20] = 0x01, ip[21] = 0x01, ip[22] = 0x01, ip[23] = 0x00;   // NOP NOP NOP EOL
  uint8_t *u = ip + ihl;
  u[0] = sport >> 8; u[1] = sport; u[2] = dport >> 8; u[3] = dport; u[4] = ulen >> 8; u[5] = ulen; u[6] = u[7] = 0;
  for (size_t i = 0; i < plen; i++) u[8 + i] = (uint8_t)(i * 7 + seed);
  if (!(fl & F_ZERO_ALL)) {
    uint16_t c = csum16(ip, ihl); if (fl & F_BAD_IP) c ^= 0x5555; ip[10] = c >> 8; ip[11] = c;
    if (!(fl & F_ZERO_UDP)) { c = udp_csum(ip, u, ulen); if (fl & F_BAD_UDP) c ^= 0x00ff; u[6] = c >> 8; u[7] = c; }
  }
  return 14 + ihl + ulen;
}

static ibv_qp *raw_qp(ibv_pd *pd, ibv_cq *cq, int nrecv)
{
  ibv_qp_init_attr ia{}; ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = 64; ia.cap.max_recv_wr = nrecv; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *qp = ibv_create_qp(pd, &ia); if (!qp) { perror("create_qp"); exit(1); }
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1;
  if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PORT)) { perror("INIT"); exit(1); }
  a = {}; a.qp_state = IBV_QPS_RTR; if (ibv_modify_qp(qp, &a, IBV_QP_STATE)) { perror("RTR"); exit(1); }
  a = {}; a.qp_state = IBV_QPS_RTS; if (ibv_modify_qp(qp, &a, IBV_QP_STATE)) { perror("RTS"); exit(1); }
  return qp;
}

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s <dev A> <netdev A> <dev B> <netdev B>\n", argv[0]); return 2; }
  char err[256]; uint8_t macA[6], macB[6]; mac_of(argv[2], macA); mac_of(argv[4], macB);
  ibv_context *ca = open_dev(argv[1], true), *cb = open_dev(argv[3], false);
  if (!ca || !cb) { fprintf(stderr, "cannot open %s (DevX) / %s\n", argv[1], argv[3]); return 1; }
  printf("A = %s (%s, %02x:%02x:%02x:%02x:%02x:%02x)  B = %s (%s, %02x:%02x:%02x:%02x:%02x:%02x)\n", argv[1], argv[2],
         macA[0], macA[1], macA[2], macA[3], macA[4], macA[5], argv[3], argv[4], macB[0], macB[1], macB[2], macB[3], macB[4], macB[5]);

  // ---------------- A. RX on port A ----------------
  printf("\n== A. RX: NIC_RX rule {IPv4, UDP, dst 192.168.77.2, dport %u, frag 0} + TAG 0x%06x -> DevX RQ\n", PORT_A, TAG);
  ibv_pd *pa = ibv_alloc_pd(ca);
  const int LOG_CQ = 6, LOG_RQ = 6, NRQ = 1 << LOG_RQ, SLOT = 2048;
  uint8_t *um = (uint8_t *)aligned_alloc(4096, 0x4000); memset(um, 0, 0x4000);
  const uint64_t CQ_OFF = 0, CQ_DBR = 0x1000, RQ_OFF = 0x2000, RQ_DBR = 0x3000;
  for (int i = 0; i < (1 << LOG_CQ); i++) um[CQ_OFF + i * 64 + 63] = 0xF1;          // invalid, owner 1
  mlx5dv_devx_umem *umem = mlx5dv_devx_umem_reg(ca, um, 0x4000, IBV_ACCESS_LOCAL_WRITE);
  uint8_t *rxb = (uint8_t *)aligned_alloc(4096, NRQ * SLOT); memset(rxb, 0, NRQ * SLOT);
  ibv_mr *rmr = ibv_reg_mr(pa, rxb, NRQ * SLOT, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(ca, MLX5DV_UAR_ALLOC_TYPE_NC);
  mlx5dv_pd dpa{}; mlx5dv_obj o{}; o.pd.in = pa; o.pd.out = &dpa;
  if (!umem || !rmr || !uar || mlx5dv_init_obj(&o, MLX5DV_OBJ_PD)) { perror("A setup"); return 1; }
  e2_rx_cfg rc{}; rc.pdn = dpa.pdn; rc.uar_page = uar->page_id; rc.umem_id = umem->umem_id;
  rc.cq_off = CQ_OFF; rc.cq_dbr_off = CQ_DBR; rc.rq_off = RQ_OFF; rc.rq_dbr_off = RQ_DBR; rc.log_cq = LOG_CQ; rc.log_rq = LOG_RQ;
  rc.ethertype = 0x0800; rc.udp = 1; rc.dst_ip = IP_A; rc.udp_dport = PORT_A; rc.tag = TAG;
  e2_rx rx;
  if (e2_rx_create(ca, &rc, &rx, err, sizeof(err))) { printf("*** %s\n", err); check(false, "5-tuple + frag + TAG rule accepted"); return 1; }
  check(true, "NIC_RX rule {ethertype, ip_protocol, dst_ipv4, udp_dport, frag} + TAG + DEVX TIR accepted (questions 2, 3)");
  for (int i = 0; i < NRQ; i++) {
    uint32_t *d = (uint32_t *)(um + RQ_OFF + i * 16);
    d[0] = htobe32(SLOT); d[1] = htobe32(rmr->lkey); *(uint64_t *)(d + 2) = htobe64((uint64_t)(uintptr_t)(rxb + i * SLOT));
  }
  _mm_sfence(); *(volatile uint32_t *)(um + RQ_DBR) = htobe32(NRQ);

  // sender on port B: verbs raw QP, frames exactly as crafted (no checksum offload requested)
  ibv_pd *pb = ibv_alloc_pd(cb); ibv_cq *bcq = ibv_create_cq(cb, 256, nullptr, nullptr, 0);
  ibv_qp *bq = raw_qp(pb, bcq, 64);
  uint8_t *txb = (uint8_t *)aligned_alloc(4096, 64 * SLOT); ibv_mr *tmr = ibv_reg_mr(pb, txb, 64 * SLOT, IBV_ACCESS_LOCAL_WRITE);
  auto send_b = [&](int slot, size_t len) {
    ibv_sge s{(uint64_t)(uintptr_t)(txb + slot * SLOT), (uint32_t)len, tmr->lkey}; ibv_send_wr w{}, *bad;
    w.wr_id = slot; w.sg_list = &s; w.num_sge = 1; w.opcode = IBV_WR_SEND; w.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(bq, &w, &bad)) { perror("post_send B"); exit(1); }
    ibv_wc wc; int n = 0; for (int t = 0; t < 1000 && (n = ibv_poll_cq(bcq, 1, &wc)) == 0; t++) usleep(100);
    if (n != 1 || wc.status) printf("  (send on B: %s)\n", n == 1 ? ibv_wc_status_str(wc.status) : "no completion");
  };
  struct Case { const char *name; int fl; uint16_t dport; size_t plen; bool deliver; int l3ok, l4ok; };
  std::vector<Case> cases = {
    {"good UDP, 64 B payload",            0,          PORT_A,     64,  true,  1, 1},
    {"good UDP, 1 B payload (padded)",    0,          PORT_A,     1,   true,  1, 1},
    {"good UDP, 1400 B payload",          0,          PORT_A,     1400, true, 1, 1},
    {"bad UDP checksum",                  F_BAD_UDP,  PORT_A,     64,  true,  1, 0},
    {"bad IP header checksum",            F_BAD_IP,   PORT_A,     64,  true,  0, -1},
    {"UDP checksum 0 (none)",             F_ZERO_UDP, PORT_A,     64,  true,  1, -1},
    {"IP options (IHL 6)",                F_OPT,      PORT_A,     64,  true,  -1, -1},
    {"other UDP port",                    0,          PORT_A + 1, 64,  false, 0, 0},
    {"IP fragment (MF set)",              F_FRAG,     PORT_A,     64,  false, 0, 0},
  };
  uint32_t ci = 0; const unsigned cqn = 1u << LOG_CQ;
  auto poll_a = [&](std::vector<std::vector<uint8_t>> &got) {
    for (;;) {
      uint8_t *c = um + CQ_OFF + (ci & (cqn - 1)) * 64; const uint8_t op_own = c[63];
      if ((op_own >> 4) == 0xF || (op_own & 1) != ((ci >> LOG_CQ) & 1)) break;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      got.emplace_back(c, c + 64); ci++;
      *(volatile uint32_t *)(um + CQ_DBR) = htobe32(ci & 0xffffff);
    }
  };
  int slot = 0;
  for (auto &k : cases) {
    size_t len = build(txb + slot * SLOT, macA, macB, IP_B, IP_A, 40000 + slot, k.dport, k.plen, k.fl, (uint8_t)(slot * 11));
    send_b(slot, len);
    usleep(20000);
    std::vector<std::vector<uint8_t>> got; poll_a(got);
    printf("  -- %s (%zu B on the wire): %zu CQE(s)\n", k.name, len < 60 ? 60 : len, got.size());
    check(got.size() == (k.deliver ? 1u : 0u), std::string(k.name) + (k.deliver ? ": delivered to the tagged RQ" : ": NOT delivered (goes to the kernel)"));
    if (got.size() == 1) {
      const uint8_t *c = got[0].data();
      const unsigned opcode = c[63] >> 4, byte_cnt = (c[44] << 24) | (c[45] << 16) | (c[46] << 8) | c[47];
      const unsigned hds = c[28], hdr = c[29], tag = (c[57] << 16) | (c[58] << 8) | c[59];
      const unsigned l2ok = hds & 1, l3ok = (hds >> 1) & 1, l4ok = (hds >> 2) & 1, l4t = (hdr >> 4) & 7, l3t = (hdr >> 2) & 3;
      const unsigned hash = (c[12] << 24) | (c[13] << 16) | (c[14] << 8) | c[15], csum = (c[20] << 8) | c[21];
      printf("     opcode %u byte_cnt %u tag 0x%06x | hds_ip_ext 0x%02x (L2 %u L3 %u L4 %u) | l4_l3_hdr 0x%02x (L4 type %u, L3 type %u) | rss_hash 0x%08x | hw csum 0x%04x\n",
             opcode, byte_cnt, tag, hds, l2ok, l3ok, l4ok, hdr, l4t, l3t, hash, csum);
      printf("     raw: "); for (int b = 0; b < 64; b++) printf("%02x%s", c[b], (b & 15) == 15 ? " " : ""); printf("\n");
      const uint8_t *fr = rxb + ((ci - 1) & (NRQ - 1)) * SLOT;
      check(opcode == 2 && byte_cnt == (len < 60 ? 60 : len), "opcode 2 (RESP_SEND), byte_cnt = frame length");
      check(!memcmp(fr, txb + slot * SLOT, len), "frame data byte-exact in the RQ buffer");
      check(tag == TAG, "flow TAG in sop_drop_qpn[23:0] (bytes 57-59) (question 2, 5)");
      if (k.l3ok >= 0) check(l3ok == (unsigned)k.l3ok, std::string("L3_OK = ") + std::to_string(k.l3ok) + " (hds_ip_ext bit 1) (question 1, 5)");
      if (k.l4ok >= 0) check(l4ok == (unsigned)k.l4ok, std::string("L4_OK = ") + std::to_string(k.l4ok) + " (hds_ip_ext bit 2) (question 1, 5)");
      if (!(k.fl & (F_BAD_IP | F_OPT))) check(l3t == 2 && l4t == 2, "header types: L3 IPv4 (2), L4 UDP (2) (l4_l3_hdr_type) (question 5)");
    }
    slot++;
  }

  // ---------------- B. TX checksum offload from port A ----------------
  printf("\n== B. TX: DevX SQ on port A, checksum fields zeroed, received on port B (dport %u)\n", PORT_TX);
  ibv_cq *acq = ibv_create_cq(ca, 64, nullptr, nullptr, 0);
  mlx5dv_cq dcq{}; o = {}; o.cq.in = acq; o.cq.out = &dcq;
  if (mlx5dv_init_obj(&o, MLX5DV_OBJ_CQ)) { perror("init_obj cq"); return 1; }
  const int LOG_WQ = 6; uint8_t *ring = (uint8_t *)aligned_alloc(4096, 0x20000); memset(ring, 0, 0x20000);
  uint8_t *wq = ring; volatile uint32_t *dbr = (volatile uint32_t *)(ring + 0x10000);
  mlx5dv_devx_umem *rumem = mlx5dv_devx_umem_reg(ca, ring, 0x20000, IBV_ACCESS_LOCAL_WRITE);
  uint8_t *pay = (uint8_t *)aligned_alloc(4096, 16 * SLOT); ibv_mr *pmr = ibv_reg_mr(pa, pay, 16 * SLOT, IBV_ACCESS_LOCAL_WRITE);
  int mi = e1_query_min_inline(ca, err, sizeof(err)); if (mi < 0) mi = 1;
  e1_sq sq;
  if (!rumem || !pmr || e1_create_sq(ca, dpa.pdn, dcq.cqn, uar->page_id, rumem->umem_id, 0, rumem->umem_id, 0x10000, LOG_WQ, mi, &sq, err, sizeof(err))) {
    printf("*** SQ: %s\n", err); return 1; }
  printf("  DevX SQ 0x%x, min_wqe_inline_mode %d\n", sq.sqn, mi);
  // receiver on B
  uint8_t *brx = (uint8_t *)aligned_alloc(4096, 64 * SLOT); memset(brx, 0, 64 * SLOT);
  ibv_mr *bmr = ibv_reg_mr(pb, brx, 64 * SLOT, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq *brcq = ibv_create_cq(cb, 128, nullptr, nullptr, 0); ibv_qp *brq = raw_qp(pb, brcq, 64);
  struct { ibv_flow_attr a; ibv_flow_spec_ipv4 i; ibv_flow_spec_tcp_udp u; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 2; fl.a.port = 1;
  fl.i.type = IBV_FLOW_SPEC_IPV4; fl.i.size = sizeof(fl.i); fl.i.val.dst_ip = htonl(IP_B); fl.i.mask.dst_ip = 0xffffffff;
  fl.u.type = IBV_FLOW_SPEC_UDP; fl.u.size = sizeof(fl.u); fl.u.val.dst_port = htons(PORT_TX); fl.u.mask.dst_port = 0xffff;
  ibv_flow *bflow = ibv_create_flow(brq, &fl.a); if (!bflow) { perror("B create_flow"); return 1; }
  for (int i = 0; i < 64; i++) {
    ibv_sge s{(uint64_t)(uintptr_t)(brx + i * SLOT), (uint32_t)SLOT, bmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = i; w.sg_list = &s; w.num_sge = 1;
    ibv_post_recv(brq, &w, &bad);
  }
  struct TxCase { const char *name; size_t plen; bool full_inline; bool offload; };
  std::vector<TxCase> tx = {
    {"42 B inline header + pointer payload, cs_flags L3|L4", 512, false, true},
    {"42 B inline header + pointer payload, 1 B payload",    1,   false, true},
    {"fully inline (120 B frame), cs_flags L3|L4",           78,  true,  true},
    {"control: 42 B inline header, NO cs_flags",             512, false, false},
  };
  uint32_t pi = 0, tci = 0;
  for (size_t t = 0; t < tx.size(); t++) {
    auto &k = tx[t]; uint8_t fr[2048];
    const size_t len = build(fr, macB, macA, IP_A, IP_B, PORT_A, PORT_TX, k.plen, F_ZERO_ALL, (uint8_t)(0x40 + t));
    uint8_t w[256] = {0}; auto *ctrl = (mlx5_wqe_ctrl_seg *)w; auto *eth = (mlx5_wqe_eth_seg *)(w + 16);
    if (k.offload) eth->cs_flags = MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;
    const size_t ih = k.full_inline ? len : 42;
    eth->inline_hdr_sz = htobe16(ih);
    memcpy(eth->inline_hdr_start, fr, ih);                    // 2 bytes in the eth segment, the rest after it
    size_t off = 16 + 16 + ((ih - 2 + 15) & ~(size_t)15);
    if (!k.full_inline) {
      memcpy(pay + t * SLOT, fr + 42, len - 42);
      mlx5dv_set_data_seg((mlx5_wqe_data_seg *)(w + off), len - 42, pmr->lkey, (uintptr_t)(pay + t * SLOT)); off += 16;
    }
    const unsigned ds = off / 16, bbs = (off + 63) / 64;
    mlx5dv_set_ctrl_seg(ctrl, pi & 0xffff, MLX5_OPCODE_SEND, 0, sq.sqn, MLX5_WQE_CTRL_CQ_UPDATE, ds, 0, 0);
    for (unsigned b = 0; b < bbs; b++) memcpy(wq + ((pi + b) & ((1u << LOG_WQ) - 1)) * 64, w + b * 64, 64);
    pi += bbs; _mm_sfence();
    dbr[MLX5_SND_DBR] = htobe32(pi & 0xffff); _mm_sfence();
    *(volatile uint64_t *)uar->reg_addr = *(uint64_t *)w; _mm_sfence();
    // TX completion
    bool done = false;
    for (int r = 0; r < 2000 && !done; r++) {
      uint8_t *p = (uint8_t *)dcq.buf + (tci & (dcq.cqe_cnt - 1)) * dcq.cqe_size + (dcq.cqe_size == 128 ? 64 : 0);
      auto *cqe = (mlx5_cqe64 *)p; const uint8_t op = cqe->op_own >> 4;
      if (op != MLX5_CQE_INVALID && (cqe->op_own & 1) == !!(tci & dcq.cqe_cnt)) {
        if (op == MLX5_CQE_REQ_ERR) { auto *e = (mlx5_err_cqe *)p; printf("  TX error CQE: syndrome 0x%x vendor 0x%x\n", e->syndrome, e->vendor_err_synd); }
        tci++; dcq.dbrec[0] = htobe32(tci & 0xffffff); done = true;
      } else usleep(100);
    }
    usleep(20000);
    ibv_wc wc; int n = ibv_poll_cq(brcq, 1, &wc);
    printf("  -- %s (%zu B, WQE %u B)\n", k.name, len, (unsigned)off);
    check(done && n == 1 && wc.status == IBV_WC_SUCCESS, "sent and received on port B");
    if (n == 1 && wc.status == IBV_WC_SUCCESS) {
      uint8_t *r = brx + wc.wr_id * SLOT, *ip = r + 14, *u = ip + 20;
      const uint16_t ipc = (ip[10] << 8) | ip[11], uc = (u[6] << 8) | u[7];
      const bool ip_ok = csum16(ip, 20) == 0, u_ok = uc != 0 && udp_valid(ip, u);
      printf("     received IP csum 0x%04x (%s), UDP csum 0x%04x (%s); payload %s\n", ipc, ip_ok ? "valid" : "INVALID", uc,
             uc == 0 ? "zero" : (u_ok ? "valid" : "INVALID"), !memcmp(r + 42, fr + 42, len - 42) ? "byte-exact" : "DIFFERS");
      if (k.offload) check(ip_ok && u_ok, "NIC filled valid IP and UDP checksums (question 4)");
      else check(ipc == 0 && uc == 0, "control: checksums still zero without cs_flags (the offload, not something else, filled them)");
      check(!memcmp(r + 42, fr + 42, len - 42), "payload byte-exact");
      ibv_sge s{(uint64_t)(uintptr_t)r, (uint32_t)SLOT, bmr->lkey}; ibv_recv_wr rw{}, *bad; rw.wr_id = wc.wr_id; rw.sg_list = &s; rw.num_sge = 1;
      ibv_post_recv(brq, &rw, &bad);
    }
  }

  printf("\nNICQ %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
  ibv_destroy_flow(bflow); ibv_destroy_qp(brq); ibv_destroy_qp(bq);
  e1_destroy_sq(&sq); e2_rx_destroy(&rx);
  return fails ? 1 : 0;
}
