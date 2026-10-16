// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// Software-simulation stand-in for hls::burst_maxi, force-included ahead of the kernels (g++ -include). The Vitis HLS
// 2023.2 C-sim model keeps its request queues in one global std::map keyed by base pointer: not thread-safe, and every
// port on the same memory shares one record. Here each port object keeps its own queue; accesses go straight to memory,
// write_response() only checks the bookkeeping. Used only by fld_sim; the hardware build uses the real header.
#ifndef X_HLS_BURST_MAXI_SIM_H
#define X_HLS_BURST_MAXI_SIM_H
#include <deque>
#include <cassert>
#include <cstring>
#include <atomic>
#include "ap_int.h"

namespace hls {
template <typename T> class burst_maxi {
public:
  burst_maxi(T *p) : Ptr(p) {}
  void read_request(size_t off, unsigned len) { assert(len > 0); rq.push_back({off, len}); }
  T read() {
    assert(!rq.empty() && "read without request");
    Req &r = rq.front();
    // Read top-down with an acquire fence after the highest word: the fake NIC writes a CQE's bytes 0..62 before its
    // owner byte 63 (release), so a reader that sees the new owner byte also sees the rest -- the hardware writes a CQE
    // as one 64-byte TLP, and a low-to-high memcpy could tear it (new owner byte, stale length).
    T v;
    uint64_t w[sizeof(T) / 8];
    const volatile uint64_t *src = reinterpret_cast<const volatile uint64_t *>(&Ptr[r.off + r.disp]);
    w[sizeof(T) / 8 - 1] = src[sizeof(T) / 8 - 1];
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int i = (int)(sizeof(T) / 8) - 2; i >= 0; i--) w[i] = src[i];
    memcpy((void *)&v, (const void *)w, sizeof(T));
    if (++r.disp == r.len) rq.pop_front();
    return v;
  }
  void write_request(size_t off, unsigned len) { assert(len > 0); wq.push_back({off, len}); }
  void write(const T &val, ap_int<sizeof(T)> be = -1) {
    assert(!wq.empty() && "write without request");
    Req &r = wq.front();
    char *d = reinterpret_cast<char *>(&Ptr[r.off + r.disp]);
    const char *s = reinterpret_cast<const char *>(&val);
    for (unsigned i = 0; i < sizeof(T); i++) if (be[i]) d[i] = s[i];
    if (++r.disp == r.len) { wq.pop_front(); resp++; }
  }
  void write_response() { assert(resp > 0 && "write_response without a completed write"); resp--; }
private:
  struct Req { size_t off; unsigned len; unsigned disp = 0; };
  T *Ptr;
  std::deque<Req> rq, wq;
  unsigned resp = 0;
};
}
#endif
