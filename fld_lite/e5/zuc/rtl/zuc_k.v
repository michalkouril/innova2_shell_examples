// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// E5.2 Vitis RTL kernel around the FlexDriver ZUC AFU (zuc_afu.v, BSD-2-Clause, (c) 2020-2021 Gabi Malka).
// User-managed control: s_axi_control is the AFU's own CSR space (CTRL0 0x1000, SCRATCHPAD 0x1030, counters 0x1100..,
// TIMESTAMP 0x1200, histograms 0x2000..), reached with xrt::ip. No ap_start: the AFU runs from reset.
//   s_axis_in   <- zuc_in  (AFU order: MSB-first, 72-bit tuser; zuc_in only forwards well-formed requests)
//   m_axis_out  -> zuc_out
// The AFU's TX handshake is not AXI-S compliant (it samples ready one cycle before it drives valid and then advances
// regardless, work/docs/zuc_afu_port_spec.md 8.2), so sbu2pci feeds a FIFO whose s_axis_tready depends only on its
// own registered occupancy; the Vitis stream sees the FIFO's compliant master side.
module zuc_k (
  input  wire         ap_clk,
  input  wire         ap_rst_n,
  // AXI4-Lite control (16-bit address: the AFU decodes [18:0], everything used lies below 0x3000)
  input  wire [15:0]  s_axi_control_AWADDR,
  input  wire         s_axi_control_AWVALID,
  output wire         s_axi_control_AWREADY,
  input  wire [31:0]  s_axi_control_WDATA,
  input  wire [3:0]   s_axi_control_WSTRB,
  input  wire         s_axi_control_WVALID,
  output wire         s_axi_control_WREADY,
  output wire [1:0]   s_axi_control_BRESP,
  output wire         s_axi_control_BVALID,
  input  wire         s_axi_control_BREADY,
  input  wire [15:0]  s_axi_control_ARADDR,
  input  wire         s_axi_control_ARVALID,
  output wire         s_axi_control_ARREADY,
  output wire [31:0]  s_axi_control_RDATA,
  output wire [1:0]   s_axi_control_RRESP,
  output wire         s_axi_control_RVALID,
  input  wire         s_axi_control_RREADY,
  // request stream (AFU byte order)
  input  wire [511:0] s_axis_in_tdata,
  input  wire [63:0]  s_axis_in_tkeep,
  input  wire [71:0]  s_axis_in_tuser,
  input  wire         s_axis_in_tlast,
  input  wire         s_axis_in_tvalid,
  output wire         s_axis_in_tready,
  // response stream (AFU byte order)
  output wire [511:0] m_axis_out_tdata,
  output wire [63:0]  m_axis_out_tkeep,
  output wire [71:0]  m_axis_out_tuser,
  output wire         m_axis_out_tlast,
  output wire         m_axis_out_tvalid,
  input  wire         m_axis_out_tready
);
  reg rst_q1 = 1'b1, rst_q2 = 1'b1;                                  // synchronised active-high reset for the AFU
  always @(posedge ap_clk) begin rst_q1 <= ~ap_rst_n; rst_q2 <= rst_q1; end

  wire         tx_vld, tx_rdy, tx_last;
  wire [511:0] tx_data;
  wire [63:0]  tx_keep;
  wire [71:0]  tx_user;

  zuc_afu afu (
    .clk(ap_clk), .reset(rst_q2), .afu_soft_reset(),
    .pci2sbu_axi4stream_rdy(s_axis_in_tready), .pci2sbu_axi4stream_vld(s_axis_in_tvalid),
    .pci2sbu_axi4stream_tdata(s_axis_in_tdata), .pci2sbu_axi4stream_tkeep(s_axis_in_tkeep),
    .pci2sbu_axi4stream_tlast(s_axis_in_tlast), .pci2sbu_axi4stream_tuser(s_axis_in_tuser),
    .sbu2pci_axi4stream_rdy(tx_rdy), .sbu2pci_axi4stream_vld(tx_vld),
    .sbu2pci_axi4stream_tdata(tx_data), .sbu2pci_axi4stream_tkeep(tx_keep),
    .sbu2pci_axi4stream_tlast(tx_last), .sbu2pci_axi4stream_tuser(tx_user),
    .axilite_aw_rdy(s_axi_control_AWREADY), .axilite_aw_vld(s_axi_control_AWVALID),
    .axilite_aw_addr({48'b0, s_axi_control_AWADDR}), .axilite_aw_prot(3'b0),
    .axilite_w_rdy(s_axi_control_WREADY), .axilite_w_vld(s_axi_control_WVALID),
    .axilite_w_data(s_axi_control_WDATA), .axilite_w_strobe(s_axi_control_WSTRB),
    .axilite_b_rdy(s_axi_control_BREADY), .axilite_b_vld(s_axi_control_BVALID), .axilite_b_resp(s_axi_control_BRESP),
    .axilite_ar_rdy(s_axi_control_ARREADY), .axilite_ar_vld(s_axi_control_ARVALID),
    .axilite_ar_addr({48'b0, s_axi_control_ARADDR}), .axilite_ar_prot(3'b0),
    .axilite_r_rdy(s_axi_control_RREADY), .axilite_r_vld(s_axi_control_RVALID),
    .axilite_r_data(s_axi_control_RDATA), .axilite_r_resp(s_axi_control_RRESP),
    .fld_pci_sample_soft_reset(), .fld_pci_sample_enable(), .afu_events()
  );

  // the safe sink: xpm_fifo_axis s_axis_tready = !full (registered). The AFU never writes on consecutive clocks, so the
  // occupancy cannot rise between the cycle it samples ready and the cycle it drives valid.
  xpm_fifo_axis #(
    .CLOCKING_MODE("common_clock"), .FIFO_DEPTH(512), .FIFO_MEMORY_TYPE("block"), .PACKET_FIFO("false"),
    .TDATA_WIDTH(512), .TUSER_WIDTH(72), .TID_WIDTH(1), .TDEST_WIDTH(1),
    .USE_ADV_FEATURES("0000"), .ECC_MODE("no_ecc"), .SIM_ASSERT_CHK(0)
  ) txq (
    .s_aclk(ap_clk), .s_aresetn(~rst_q2), .m_aclk(ap_clk),
    .s_axis_tvalid(tx_vld), .s_axis_tready(tx_rdy), .s_axis_tdata(tx_data),
    .s_axis_tkeep(tx_keep), .s_axis_tstrb(tx_keep), .s_axis_tlast(tx_last), .s_axis_tuser(tx_user),
    .s_axis_tid(1'b0), .s_axis_tdest(1'b0),
    .m_axis_tvalid(m_axis_out_tvalid), .m_axis_tready(m_axis_out_tready), .m_axis_tdata(m_axis_out_tdata),
    .m_axis_tkeep(m_axis_out_tkeep), .m_axis_tstrb(), .m_axis_tlast(m_axis_out_tlast), .m_axis_tuser(m_axis_out_tuser),
    .m_axis_tid(), .m_axis_tdest(),
    .wr_data_count_axis(), .rd_data_count_axis(), .prog_full_axis(), .prog_empty_axis(),
    .almost_full_axis(), .almost_empty_axis(), .injectsbiterr_axis(1'b0), .injectdbiterr_axis(1'b0),
    .sbiterr_axis(), .dbiterr_axis()
  );
endmodule
