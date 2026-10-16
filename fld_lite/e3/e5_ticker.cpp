// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// E5.0 ticker (2026-09-30): a free-running kernel (ap_ctrl_none, never started or stopped by XRT) holding a cycle
// counter. Every request arriving on req is answered on tick with the current count. Joined to e3_echo6 by
// stream_connect; see e3_echo6.cpp.
#include <ap_int.h>
#include <hls_stream.h>
#include <ap_axi_sdata.h>
typedef ap_axiu<16, 0, 0, 0> req_t;
typedef ap_axiu<64, 0, 0, 0> tick_t;

extern "C" void e5_ticker(hls::stream<req_t> &req, hls::stream<tick_t> &tick)
{
#pragma HLS INTERFACE axis port=req
#pragma HLS INTERFACE axis port=tick
#pragma HLS INTERFACE ap_ctrl_none port=return
  static ap_uint<64> c = 0;
tk:
  while (1) {
#pragma HLS PIPELINE II=1
    c++;
    req_t r;
    if (req.read_nb(r)) { tick_t t; t.data = c; t.keep = -1; t.strb = -1; t.last = 1; tick.write(t); }
  }
}
