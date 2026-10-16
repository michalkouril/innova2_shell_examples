// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E3 (2026-09-29, E3 dev shell): the e3_tx RP kernel transmits on ConnectX-5 port 0 with no CPU in the data path --
// it writes the SEND WQEs and the SQ doorbell record into FPGA DDR and rings the ConnectX doorbell itself through the
// shell's PEER port (peer_window -> XDMA S_AXI_B). The host only sets things up (DevX SQ + CQ in FPGA DDR, the peer
// window pointed at the UAR page) and, on port 1 (cable loop), receives and checks every frame byte for byte.
//   usage (root): e3_host <user BDF> <e3 xclbin> <tx dev> <rx dev> <cx5 BDF of tx dev> [frames] [--no-window]
//   --no-window  negative control: leave the peer window disabled (doorbells are refused, nothing may be sent)
//   --win N      E3.1 (peer_window v1.1): ring through window N (0..3); the other windows stay at base 0 = refused
#include <xrt/xrt_device.h>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_kernel.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <immintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include "devx_sq.h"
#include "devx_cq.h"

static const size_t BUF = 8 << 20, FRAME = 1024, STRIDE = 2048, NFR = 1024;
static const size_t DATA_OFF = 0, UMEM_OFF = 0x200000, UMEM_LEN = 0x40000;
static const size_t SQ_OFF = 0x0, SQ_DBR = 0x10000, CQ_DBR = 0x10040, CQ_OFF = 0x20000;   // inside the umem
static const int LOG_SQ = 10, LOG_CQ = 11;
static const uint16_t ETYPE = 0x88B7;
static const size_t PW_OFF = 0x100000;                     // peer_window ctl in the user PF's BAR0

static bool has(int argc, char **argv, const char *f) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], f)) return true; return false; }
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
static uint64_t bar_start(const std::string &bdf, int bar)
{
  std::ifstream f("/sys/bus/pci/devices/" + bdf + "/resource"); std::string l; uint64_t s = 0;
  for (int i = 0; i <= bar && std::getline(f, l); i++) s = strtoull(l.c_str(), nullptr, 16);
  return s;
}
static void make_frame(uint8_t *f, unsigned i)
{
  memset(f, 0xFF, 6); memcpy(f + 6, "\x02\x49\x4e\x4e\x32\x03", 6); f[12] = ETYPE >> 8; f[13] = ETYPE & 0xFF;
  for (size_t k = 14; k < FRAME; k++) f[k] = (uint8_t)(k * 11 + i * 7);
  f[14] = 0xE3; f[15] = i & 0xFF; f[16] = (i >> 8) & 0xFF;
}

int main(int argc, char **argv)
{
  if (argc < 6) { fprintf(stderr, "usage: %s <user BDF> <xclbin> <tx dev> <rx dev> <cx5 BDF> [frames] [--no-window]\n", argv[0]); return 2; }
  unsigned n = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 1000;
  bool window = !has(argc, argv, "--no-window");
  unsigned win = 0;
  for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--win")) win = atoi(argv[i + 1]) & 3;
  char err[256];

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel krnl(dev, uuid, "e3_tx");
  xrt::bo bo(dev, BUF, xrt::bo::flags::p2p, krnl.group_id(0));
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, krnl.group_id(1));   // PEER: gives the kernel its address; never synced
  xrt::bo res(dev, 4096, xrt::bo::flags::normal, krnl.group_id(2));
  uint8_t *base = bo.map<uint8_t *>(), *um = base + UMEM_OFF;
  printf("E3: frames, SQ ring + doorbell record, TX CQ in FPGA DDR (bank0 BO 0x%llx); PEER BO at 0x%llx\n",
         (unsigned long long)bo.address(), (unsigned long long)pbo.address());

  // ConnectX: SQ + CQ in FPGA DDR
  ibv_context *ct = open_dev(argv[3], true), *cr = open_dev(argv[4], false);
  if (!ct || !cr) { fprintf(stderr, "cannot open %s / %s\n", argv[3], argv[4]); return 1; }
  ibv_pd *pd = ibv_alloc_pd(ct);
  ibv_mr *dmr = ibv_reg_mr(pd, base + DATA_OFF, NFR * STRIDE, IBV_ACCESS_LOCAL_WRITE);
  mlx5dv_devx_umem *umem = mlx5dv_devx_umem_reg(ct, um, UMEM_LEN, IBV_ACCESS_LOCAL_WRITE);
  if (!dmr || !umem) { printf("registration failed: %s (is innova2_peermem loaded?)\n", strerror(errno)); return 1; }
  mlx5dv_pd dpd{}; mlx5dv_obj o{}; o.pd.in = pd; o.pd.out = &dpd; mlx5dv_init_obj(&o, MLX5DV_OBJ_PD);
  mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(ct, MLX5DV_UAR_ALLOC_TYPE_NC);
  memset(um, 0, UMEM_LEN);
  for (unsigned i = 0; i < (1u << LOG_CQ); i++) um[CQ_OFF + i * 64 + 63] = 0xF1;
  for (unsigned i = 0; i < NFR; i++) make_frame(base + DATA_OFF + i * STRIDE, i);
  _mm_sfence();
  e3_cq cq; e1_sq sq;
  if (e3_cq_create(ct, uar->page_id, umem->umem_id, CQ_OFF, CQ_DBR, LOG_CQ, &cq, err, sizeof(err))) { printf("*** %s\n", err); return 1; }
  int mi = e1_query_min_inline(ct, err, sizeof(err)); if (mi < 0) mi = 1;
  if (e1_create_sq(ct, dpd.pdn, cq.cqn, uar->page_id, umem->umem_id, SQ_OFF, umem->umem_id, SQ_DBR, LOG_SQ, mi, &sq, err, sizeof(err))) { printf("*** %s\n", err); return 1; }

  // the doorbell register's PCIe bus address, and the peer window pointed at it
  const uint64_t uar_bus = bar_start(argv[5], 0) + (uint64_t)uar->page_id * 4096 + ((uint8_t *)uar->reg_addr - (uint8_t *)uar->base_addr);
  const uint64_t wbase = uar_bus & ~0xFFFFFull;
  const unsigned db_off = (unsigned)((uar_bus & 0xFFFFF) + (win << 20) - (pbo.address() & 0xFFFFF));
  static const unsigned BREG[4] = {0x08, 0x28, 0x30, 0x38};   // BASEn lo register offsets (hi = +4)
  int fd = open(("/sys/bus/pci/devices/" + std::string(argv[1]) + "/resource0").c_str(), O_RDWR | O_SYNC);
  volatile uint32_t *pw = (volatile uint32_t *)((uint8_t *)mmap(nullptr, 1 << 25, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) + PW_OFF);
  printf("DevX: cqn 0x%x sqn 0x%x uar page %u -> doorbell bus 0x%llx; peer_window ID 0x%08x WIN_LOG2 %u\n", cq.cqn, sq.sqn,
         uar->page_id, (unsigned long long)uar_bus, pw[0], pw[4]);
  if (pw[0] != 0x50454552) { printf("*** peer_window not found at user BAR0+0x%zx -- is the E3 shell running?\n", PW_OFF); return 1; }
  pw[1] = 0;
  if (win > 0 && pw[8] < 4) { printf("*** --win %u needs peer_window v1.1 (NWIN reads %u)\n", win, pw[8]); return 1; }
  if (win > 0) for (unsigned w = 0; w < 4; w++) { pw[BREG[w] / 4] = 0; pw[BREG[w] / 4 + 1] = 0; }   // only window 'win' valid
  pw[BREG[win] / 4] = (uint32_t)wbase; pw[BREG[win] / 4 + 1] = (uint32_t)(wbase >> 32);
  const uint32_t w0 = pw[5], r0 = pw[7];
  if (window) pw[1] = 1;
  printf("peer window %u: base 0x%08x%08x enable %u; kernel writes PEER offset 0x%x\n", win, pw[BREG[win] / 4 + 1], pw[BREG[win] / 4], pw[1], db_off);

  // independent receiver on the other port (host memory)
  ibv_pd *pr = ibv_alloc_pd(cr);
  uint8_t *rx = (uint8_t *)aligned_alloc(4096, 1024 * FRAME);
  ibv_mr *rmr = ibv_reg_mr(pr, rx, 1024 * FRAME, IBV_ACCESS_LOCAL_WRITE);
  ibv_cq *rcq = ibv_create_cq(cr, 2048, nullptr, nullptr, 0);
  ibv_qp_init_attr ia{}; ia.send_cq = rcq; ia.recv_cq = rcq; ia.qp_type = IBV_QPT_RAW_PACKET;
  ia.cap.max_send_wr = 1; ia.cap.max_recv_wr = 1024; ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
  ibv_qp *rq = ibv_create_qp(pr, &ia);
  ibv_qp_attr a{}; a.qp_state = IBV_QPS_INIT; a.port_num = 1; ibv_modify_qp(rq, &a, IBV_QP_STATE | IBV_QP_PORT);
  a = {}; a.qp_state = IBV_QPS_RTR; ibv_modify_qp(rq, &a, IBV_QP_STATE);
  struct { ibv_flow_attr a; ibv_flow_spec_eth e; } __attribute__((packed)) fl{};
  fl.a.type = IBV_FLOW_ATTR_NORMAL; fl.a.size = sizeof(fl); fl.a.num_of_specs = 1; fl.a.port = 1;
  fl.e.type = IBV_FLOW_SPEC_ETH; fl.e.size = sizeof(fl.e); fl.e.val.ether_type = htons(ETYPE); fl.e.mask.ether_type = 0xFFFF;
  ibv_flow *flow = ibv_create_flow(rq, &fl.a);
  auto post_rx = [&](unsigned slot) {
    ibv_sge s{(uint64_t)(rx + slot * FRAME), (uint32_t)FRAME, rmr->lkey}; ibv_recv_wr w{}, *bad; w.wr_id = slot; w.sg_list = &s; w.num_sge = 1;
    ibv_post_recv(rq, &w, &bad);
  };
  for (unsigned i = 0; i < 1024; i++) post_rx(i);
  if (!flow) { perror("create_flow"); return 1; }

  // go: from here on the CPU only watches the receiver
  auto t0 = std::chrono::steady_clock::now();
  auto run = krnl(bo, pbo, res, (unsigned)(UMEM_OFF + SQ_OFF), (unsigned)LOG_SQ, (unsigned)(UMEM_OFF + SQ_DBR),
                  (unsigned)(UMEM_OFF + CQ_OFF), (unsigned)LOG_CQ, (unsigned)(UMEM_OFF + CQ_DBR), (unsigned)DATA_OFF,
                  (unsigned)STRIDE, (unsigned)NFR, (unsigned)FRAME, sq.sqn, dmr->lkey,
                  (unsigned long long)(uintptr_t)(base + DATA_OFF), db_off, n, window ? 400000000u : 2000000u);
  unsigned got = 0, good = 0, rxerr = 0;
  std::vector<uint8_t> ref(FRAME);
  for (int idle = 0; got < n && idle < 3000; ) {
    ibv_wc wc[32]; int k = ibv_poll_cq(rcq, 32, wc);
    if (k <= 0) { idle++; usleep(1000); continue; }
    idle = 0;
    for (int j = 0; j < k; j++) {
      unsigned slot = wc[j].wr_id;
      if (wc[j].status) rxerr++;
      else { make_frame(ref.data(), got % NFR); good += wc[j].byte_len == FRAME && !memcmp(rx + slot * FRAME, ref.data(), FRAME); }
      got++; post_rx(slot);
    }
  }
  auto st = run.wait(std::chrono::milliseconds(20000));
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  res.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  uint32_t r[8]; res.read(r, sizeof(r), 0);
  printf("kernel (%s): WQEs posted %u, CQEs %u, error CQEs %u, polls %u%s\n", st == ERT_CMD_STATE_COMPLETED ? "done" : "NOT done",
         r[0], r[1], r[2], r[3], r[2] ? "" : "");
  if (r[2]) printf("first error CQE: opcode %u syndrome 0x%02x vendor 0x%02x\n", r[4] >> 28, (r[4] >> 16) & 0xFF, (r[4] >> 8) & 0xFF);
  printf("peer window: writes forwarded %u (doorbells), refused %u\n", pw[5] - w0, pw[7] - r0);
  printf("receiver on %s: frames %u/%u, byte-exact %u, errors %u  (%.3f s, %.0f frames/s)\n", argv[4], got, n, good, rxerr, secs, got / secs);
  bool pass = window ? (got == n && good == n && !rxerr && r[0] == n && r[1] == n && !r[2] && pw[5] - w0 == n)
                     : (got == 0 && pw[5] - w0 == 0);
  printf("RESULT %s (%s)\n", pass ? "PASS" : "FAIL", window ? "E3: kernel posts WQEs and rings the ConnectX doorbell itself"
                                                          : "negative control: window disabled, nothing may be sent");
  pw[1] = 0;
  ibv_destroy_flow(flow); ibv_destroy_qp(rq); ibv_destroy_cq(rcq); ibv_dereg_mr(rmr); ibv_dealloc_pd(pr);
  e1_destroy_sq(&sq); e3_cq_destroy(&cq); mlx5dv_devx_free_uar(uar); mlx5dv_devx_umem_dereg(umem); ibv_dereg_mr(dmr);
  ibv_dealloc_pd(pd); ibv_close_device(ct); ibv_close_device(cr);
  return pass ? 0 : 1;
}
