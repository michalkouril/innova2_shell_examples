// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// gzip_xn_host <xclbin> <input> [cus] [device index] -- GZip-compress one file with several xilGzipCompBlock compute
// units at once (compression/gzip_x4: four 2-core CUs). The input is cut into one part per CU; each CU compresses its
// part into a complete gzip member (the stock host's framing: 12-byte header, the kernel's deflate blocks, a final empty
// stored block, CRC32 and ISIZE), and the members are concatenated into <input>.gz -- a multi-member gzip file, which
// RFC 1952 allows and gzip -d expands into the original. Prints the kernel throughput (input bytes over the time from
// the first start to the last completion) and the per-CU times.
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static constexpr size_t BLOCK = 32 * 1024;   // the kernel's deflate block size (BLOCKSIZE_IN_KB)

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <xclbin> <input> [cus=4] [device index=0]\n", argv[0]); return 2; }
  const std::string xb = argv[1], inPath = argv[2];
  const unsigned ncu = argc > 3 ? atoi(argv[3]) : 4, devIdx = argc > 4 ? atoi(argv[4]) : 0;
  std::ifstream f(inPath, std::ios::binary | std::ios::ate);
  if (!f) { perror(inPath.c_str()); return 1; }
  const size_t n = f.tellg(); f.seekg(0);
  std::vector<char> in(n); f.read(in.data(), n);
  if (n == 0 || n >= (1ull << 32)) { fprintf(stderr, "*** input must be 1 byte .. 4 GB\n"); return 1; }

  xrt::device dev(devIdx);
  auto uuid = dev.load_xclbin(xb);
  // parts: whole 32 KB blocks, as even as possible; fewer parts than CUs for small inputs
  const size_t nblk = (n + BLOCK - 1) / BLOCK, np = std::min<size_t>(ncu, nblk);
  struct Part { size_t off, len; xrt::kernel k; xrt::bo bi, bo, bs, bc; xrt::run r; double ms; };
  std::vector<Part> p(np);
  for (size_t i = 0, b0 = 0; i < np; i++) {
    const size_t b1 = nblk * (i + 1) / np;
    p[i].off = b0 * BLOCK; p[i].len = std::min(n, b1 * BLOCK) - p[i].off; b0 = b1;
    const std::string cu = "xilGzipCompBlock:{xilGzipCompBlock_" + std::to_string(i + 1) + "}";
    p[i].k = xrt::kernel(dev, uuid, cu);
    const size_t outCap = p[i].len + p[i].len / 8 + 64 * 1024, nb = (p[i].len + BLOCK - 1) / BLOCK;
    p[i].bi = xrt::bo(dev, p[i].len, p[i].k.group_id(0));
    p[i].bo = xrt::bo(dev, outCap, p[i].k.group_id(1));
    p[i].bs = xrt::bo(dev, 2 * nb * sizeof(uint32_t), p[i].k.group_id(2));
    p[i].bc = xrt::bo(dev, sizeof(uint32_t), p[i].k.group_id(3));
    p[i].bi.write(in.data() + p[i].off); p[i].bi.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    const uint32_t init = ~0u; p[i].bc.write(&init); p[i].bc.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    p[i].r = xrt::run(p[i].k);
    p[i].r.set_arg(0, p[i].bi); p[i].r.set_arg(1, p[i].bo); p[i].r.set_arg(2, p[i].bs); p[i].r.set_arg(3, p[i].bc);
    p[i].r.set_arg(4, (uint32_t)p[i].len); p[i].r.set_arg(5, true);                       // true: CRC32 (GZip)
  }
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  for (auto &q : p) q.r.start();
  for (auto &q : p) { q.r.wait(); q.ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }
  const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();

  std::ofstream o(inPath + ".gz", std::ios::binary);
  size_t total = 0;
  for (auto &q : p) {
    uint32_t sz, crc;
    q.bs.sync(XCL_BO_SYNC_BO_FROM_DEVICE); q.bs.read(&sz, sizeof sz, 0);
    q.bc.sync(XCL_BO_SYNC_BO_FROM_DEVICE); q.bc.read(&crc);
    if (sz == 0 || sz > q.bo.size()) { fprintf(stderr, "*** CU reported %u compressed bytes\n", sz); return 1; }
    std::vector<char> c(sz); q.bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE, sz, 0); q.bo.read(c.data(), sz, 0);
    // the stock host's header: magic, deflate, FNAME, mtime 0, XFL 0, OS 3 (Unix), file name "x"
    const unsigned char hdr[12] = {0x1f, 0x8b, 0x08, 0x08, 0, 0, 0, 0, 0x00, 0x03, 'x', 0};
    const unsigned char last[5] = {0x01, 0x00, 0x00, 0xff, 0xff};                         // final empty stored block
    const uint32_t isz = (uint32_t)q.len;
    o.write((const char *)hdr, 12); o.write(c.data(), sz); o.write((const char *)last, 5);
    o.write((const char *)&crc, 4); o.write((const char *)&isz, 4);                     // little-endian host
    total += 12 + sz + 5 + 8;
  }
  printf("GZIP-XN: %zu CUs, %zu bytes -> %zu (CR %.2f), %.2f ms, KT(MBps) :%.2f\n", np, n, total, (double)n / total, ms,
         n / ms / 1000.0);
  for (size_t i = 0; i < np; i++) printf("  CU %zu: %zu bytes, done at %.2f ms\n", i + 1, p[i].len, p[i].ms);
  return 0;
}
