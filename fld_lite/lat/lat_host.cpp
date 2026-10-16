// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Host for lat_probe: per-operation memory latency of an RP kernel, in ns and in kernel cycles.
//   usage: lat_host <user BDF> <xclbin> [reps]     |     lat_host <user BDF> <xclbin> once <mode> <off> <n>
// One 4 MB bank0 buffer, allocated first so it starts at bank0 offset 0: offset 0 is the HYBRID UltraRAM window,
// offset 2 MB is DDR. Launch overhead cancels: time(n2) - time(n1) over (n2 - n1) operations, median of `reps`.
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static double run_us(xrt::kernel &k, xrt::bo &bo, xrt::bo &res, unsigned off, unsigned n, unsigned mode)
{
  auto t0 = std::chrono::steady_clock::now();
  auto r = k(bo, res, off, n, mode);
  r.wait();
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

static double per_op_ns(xrt::kernel &k, xrt::bo &bo, xrt::bo &res, unsigned off, unsigned n1, unsigned n2, unsigned mode, int reps)
{
  std::vector<double> v;
  for (int i = 0; i < reps; i++) {
    const double a = run_us(k, bo, res, off, n1, mode), b = run_us(k, bo, res, off, n2, mode);
    v.push_back((b - a) * 1000.0 / (n2 - n1));
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: %s <user BDF> <xclbin> [reps]\n", argv[0]); return 2; }
  const int reps = argc > 3 && std::string(argv[3]) != "once" ? atoi(argv[3]) : 7;
  xrt::device dev{std::string(argv[1])};
  auto uuid = dev.load_xclbin(std::string(argv[2]));
  const char *kn = getenv("LAT_K") ? getenv("LAT_K") : "lat_probe";   // LAT_K: kernel name (e.g. lat_probe_l1)
  xrt::kernel k(dev, uuid, kn);
  printf("kernel %s\n", kn);
  xrt::bo bo(dev, 4u << 20, xrt::bo::flags::normal, k.group_id(0));
  xrt::bo res(dev, 4096, xrt::bo::flags::normal, k.group_id(1));
  if (argc > 6 && std::string(argv[3]) == "once") {   // for ILA captures: one run of <mode> at <off>, <n> operations
    auto r = k(bo, res, (unsigned)strtoul(argv[5], nullptr, 0), (unsigned)atoi(argv[6]), (unsigned)atoi(argv[4]));
    r.wait(); printf("once: mode %s off %s n %s done\n", argv[4], argv[5], argv[6]); return 0;
  }
  printf("buffer at 0x%llx (%s)\n", (unsigned long long)bo.address(),
         bo.address() == 0x200000000ull ? "bank0 offset 0: offset 0 is the URAM window in HYBRID mode" : "NOT at bank0 offset 0");
  // kernel clock: spin loop, II=1
  const double ns_per_cycle = per_op_ns(k, bo, res, 0, 1000000, 101000000, 2, reps);
  const double mhz = 1000.0 / ns_per_cycle;
  const double empty = per_op_ns(k, bo, res, 0, 10000, 1010000, 3, reps);
  printf("kernel clock: %.1f MHz (%.3f ns/cycle); empty loop iteration %.1f ns = %.1f cycles\n", mhz, ns_per_cycle, empty, empty / ns_per_cycle);
  struct { const char *name; unsigned off, mode; } t[] = {
    {"write+ack  URAM window (off 0)", 0, 0}, {"read       URAM window (off 0)", 0, 1},
    {"write+ack  DDR (off 2 MB)", 2u << 20, 0}, {"read       DDR (off 2 MB)", 2u << 20, 1}};
  for (auto &x : t) {
    const double ns = per_op_ns(k, bo, res, x.off, 10000, 210000, x.mode, reps);
    printf("%-32s %7.1f ns/op = %6.1f cycles  (minus loop overhead: %6.1f cycles)\n", x.name, ns, ns / ns_per_cycle, (ns - empty) / ns_per_cycle);
  }
  return 0;
}
