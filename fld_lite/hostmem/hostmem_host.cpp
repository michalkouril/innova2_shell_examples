// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Tier-1 host memory access: an RP kernel (hostmem_probe, sp=peer) reads and writes HOST RAM through peer_window
// windows 2 and 3, pointed at 1 MB DMA-coherent buffers from innova2_hostmem.ko. Windows 0/1 (FLD-lite / generator
// doorbells) are left alone and restored state is reported.
//   usage (root): hostmem_host <user BDF> <xclbin>
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct hostmem_info { uint32_t nbuf, size; uint64_t bus[3]; };
#define INNOVA2_HOSTMEM_INFO _IOR('h', 1, struct hostmem_info)
static const size_t PW_OFF = 0x100000;
static const unsigned MB = 1u << 20, BEATS = MB / 32;

static uint32_t pat(uint32_t p, unsigned i, unsigned k) { return p ^ ((8 * i + k) * 0x9E3779B9u); }

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: %s <user BDF> <xclbin>\n", argv[0]); return 2; }
  int hfd = open("/dev/innova2_hostmem", O_RDWR);
  if (hfd < 0) { perror("*** /dev/innova2_hostmem (insmod innova2_hostmem.ko?)"); return 1; }
  hostmem_info in{};
  if (ioctl(hfd, INNOVA2_HOSTMEM_INFO, &in)) { perror("*** ioctl"); return 1; }
  uint32_t *hb[2];
  for (int b = 0; b < 2; b++) {
    hb[b] = (uint32_t *)mmap(nullptr, MB, PROT_READ | PROT_WRITE, MAP_SHARED, hfd, (off_t)b * MB);
    if (hb[b] == MAP_FAILED) { perror("*** mmap"); return 1; }
  }
  printf("host buffers: 0 at bus 0x%llx, 1 at bus 0x%llx (1 MB each, DMA-coherent)\n", (unsigned long long)in.bus[0], (unsigned long long)in.bus[1]);

  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  xrt::kernel k(dev, uuid, "hostmem_probe");
  xrt::bo pbo(dev, 4096, xrt::bo::flags::normal, k.group_id(0));        // locates the PEER aperture
  xrt::bo res(dev, 4096, xrt::bo::flags::normal, k.group_id(1));
  printf("PEER aperture at 0x%llx; window 2 = +2 MB, window 3 = +3 MB\n", (unsigned long long)pbo.address());

  int fd = open(("/sys/bus/pci/devices/" + std::string(argv[1]) + "/resource0").c_str(), O_RDWR | O_SYNC);
  volatile uint32_t *pw = (volatile uint32_t *)((uint8_t *)mmap(nullptr, 1 << 25, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) + PW_OFF);
  if (pw[0] != 0x50454552 || pw[8] < 4) { printf("*** peer_window v1.1 (4 windows) not found\n"); return 1; }
  const uint32_t ctrl0 = pw[1], s2l = pw[0x30 / 4], s2h = pw[0x34 / 4], s3l = pw[0x38 / 4], s3h = pw[0x3C / 4];
  const uint32_t wf0 = pw[5], rf0 = pw[6], rr0 = pw[7];
  pw[1] = 0;                                                            // bases are written only while disabled
  pw[0x30 / 4] = (uint32_t)in.bus[0]; pw[0x34 / 4] = (uint32_t)(in.bus[0] >> 32);
  pw[0x38 / 4] = (uint32_t)in.bus[1]; pw[0x3C / 4] = (uint32_t)(in.bus[1] >> 32);
  pw[1] = 1;
  if (pw[0x30 / 4] != (uint32_t)(in.bus[0] & ~0xFFFFFull)) { printf("*** window 2 base did not stick (0x%08x)\n", pw[0x30 / 4]); return 1; }
  if (in.bus[0] & 0xFFFFF || in.bus[1] & 0xFFFFF) printf("NOTE: buffer not 1 MB aligned; the window drops the low 20 bits, offsets compensate\n");
  // kernel offsets are relative to pbo, which need not sit at the start of the 4 MB aperture
  const unsigned pofs = (unsigned)(pbo.address() & 0x3FFFFF);
  const unsigned o2 = 2 * MB + (unsigned)(in.bus[0] & 0xFFFFF) - pofs, o3 = 3 * MB + (unsigned)(in.bus[1] & 0xFFFFF) - pofs;
  // (the kernel can only reach the part of each buffer that lies inside its 1 MB window)
  const unsigned n2 = (MB - (unsigned)(in.bus[0] & 0xFFFFF)) / 32, n3 = (MB - (unsigned)(in.bus[1] & 0xFFFFF)) / 32;

  auto run = [&](unsigned off, unsigned n, unsigned mode, unsigned p) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = k(pbo, res, off, n, mode, p); r.wait();
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  };
  uint32_t rv[16];
  bool pass = true;

  // 1. kernel writes host RAM (window 2 -> buffer 0); the host checks every word
  memset(hb[0], 0, MB);
  const uint32_t P = 0xC0FFEE11u;
  run(o2, n2, 0, P);
  unsigned bad = 0;
  for (unsigned i = 0; i < n2; i++) for (unsigned w = 0; w < 8; w++) if (hb[0][(in.bus[0] & 0xFFFFF) / 4 + 8 * i + w] != pat(P, i, w)) bad++;
  printf("1. kernel WRITE -> host RAM: %u beats, %u wrong words  %s\n", n2, bad, bad ? "FAIL" : "PASS"); pass &= !bad;

  // 2. host writes random data (buffer 1); the kernel reads it through window 3 and XOR-folds it
  std::mt19937 rng(42); uint32_t want[8] = {0};
  for (unsigned i = 0; i < n3 * 8; i++) { const uint32_t v = rng(); hb[1][(in.bus[1] & 0xFFFFF) / 4 + i] = v; want[i % 8] ^= v; }
  __sync_synchronize();
  run(o3, n3, 1, 0); res.sync(XCL_BO_SYNC_BO_FROM_DEVICE); res.read(rv, sizeof(rv), 0);
  bool ok = true; for (int w = 0; w < 8; w++) ok &= rv[w] == want[w];
  printf("2. kernel READ  <- host RAM: %u beats, checksum %s (got %08x.. want %08x..)\n", n3, ok ? "PASS" : "FAIL", rv[0], want[0]); pass &= ok;

  // 3. bandwidth: launch overhead cancels (n large - n small)
  const unsigned nl = n2, ns = 1024;
  double tw = 1e9, tr = 1e9;
  for (int r = 0; r < 5; r++) { tw = std::min(tw, run(o2, nl, 0, P) - run(o2, ns, 0, P)); tr = std::min(tr, run(o3, n3, 1, 0) - run(o3, ns, 1, 0)); }
  printf("3. bandwidth: kernel->host write %.2f GB/s, host->kernel read %.2f GB/s (%u KB, best of 5)\n",
         (nl - ns) * 32.0 / tw / 1e3, (n3 - ns) * 32.0 / tr / 1e3, nl * 32 / 1024);

  // 4. latency of one dependent read from host RAM
  const unsigned m = 20000; double tl = 1e9, te = 1e9;
  for (int r = 0; r < 5; r++) { tl = std::min(tl, run(o3, m, 2, 0) - run(o3, 1000, 2, 0)); te = std::min(te, run(o3, m, 3, 0) - run(o3, 1000, 3, 0)); }
  printf("4. latency: dependent read from host RAM %.0f ns (loop overhead %.1f ns subtracted)\n", (tl - te) / (m - 1000) * 1e3, te / (m - 1000) * 1e3);

  printf("peer_window counters (this run): writes forwarded %u, reads forwarded %u, refused %u\n", pw[5] - wf0, pw[6] - rf0, pw[7] - rr0);
  pw[1] = 0; pw[0x30 / 4] = s2l; pw[0x34 / 4] = s2h; pw[0x38 / 4] = s3l; pw[0x3C / 4] = s3h; pw[1] = ctrl0;
  printf("windows 2/3 restored, CTRL %u\nRESULT %s (tier-1 host memory access)\n", ctrl0, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
