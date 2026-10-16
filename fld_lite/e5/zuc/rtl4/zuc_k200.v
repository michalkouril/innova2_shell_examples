// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// E5.2 Vitis RTL kernel "zuc_k", 200 MHz variant: the FlexDriver ZUC AFU runs at its design clock (200 MHz, as in the
// paper) on an MMCM derived from ap_clk (250 MHz); everything else is as zuc_k.v.
//   s_axis_in  -> xpm_fifo_axis (independent clocks) -> AFU pci2sbu
//   AFU sbu2pci -> xpm_fifo_axis (independent clocks; s_axis_tready = !full of its own write side, which is what the
//                  AFU's non-compliant TX handshake needs, zuc_afu_port_spec.md 8.2) -> m_axis_out
//   s_axi_control -> axil_cdc (one xpm_fifo_async per AXI-Lite channel) -> AFU CSRs
// Module name is zuc_k so the host, link config and kernel name are unchanged.
module zuc_k (
  input  wire         ap_clk,
  input  wire         ap_rst_n,
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
  input  wire [511:0] s_axis_in_tdata,
  input  wire [63:0]  s_axis_in_tkeep,
  input  wire [71:0]  s_axis_in_tuser,
  input  wire         s_axis_in_tlast,
  input  wire         s_axis_in_tvalid,
  output wire         s_axis_in_tready,
  output wire [511:0] m_axis_out_tdata,
  output wire [63:0]  m_axis_out_tkeep,
  output wire [71:0]  m_axis_out_tuser,
  output wire         m_axis_out_tlast,
  output wire         m_axis_out_tvalid,
  input  wire         m_axis_out_tready
);
  // ---------------------------------------------------------------- 200 MHz from ap_clk: VCO 250*24/5 = 1200, /6 = 200
  wire clk_fb, clk200_u, clk200, locked;
  MMCME4_BASE #(
    .CLKIN1_PERIOD(4.000), .DIVCLK_DIVIDE(5), .CLKFBOUT_MULT_F(24.000), .CLKOUT0_DIVIDE_F(6.000),
    .CLKOUT0_DUTY_CYCLE(0.5), .CLKOUT0_PHASE(0.0), .STARTUP_WAIT("FALSE")
  ) mmcm (
    .CLKIN1(ap_clk), .CLKFBIN(clk_fb), .CLKFBOUT(clk_fb), .CLKOUT0(clk200_u), .LOCKED(locked),
    .RST(1'b0), .PWRDWN(1'b0),
    .CLKFBOUTB(), .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(), .CLKOUT3B(),
    .CLKOUT4(), .CLKOUT5(), .CLKOUT6()
  );
  BUFG bufg200 (.I(clk200_u), .O(clk200));

  // resets: ap domain (for the FIFOs' write/read sides) and the AFU domain (held until the MMCM locks)
  reg ap_rst_q = 1'b1;
  always @(posedge ap_clk) ap_rst_q <= ~ap_rst_n;
  wire afu_rst_async = ~ap_rst_n | ~locked;
  wire afu_rst;
  xpm_cdc_async_rst #(.DEST_SYNC_FF(4), .RST_ACTIVE_HIGH(1)) rst200 (.src_arst(afu_rst_async), .dest_clk(clk200), .dest_arst(afu_rst));
  // the FIFOs reset from the ap domain; they need their reset held while the MMCM is not yet locked too
  wire fifo_rst_n = ~(ap_rst_q | ~locked);

  // ---------------------------------------------------------------- request stream: ap_clk -> clk200
  wire         in_vld, in_rdy, in_last;
  wire [511:0] in_data;
  wire [63:0]  in_keep;
  wire [71:0]  in_user;
  xpm_fifo_axis #(
    .CLOCKING_MODE("independent_clock"), .FIFO_DEPTH(64), .FIFO_MEMORY_TYPE("block"), .PACKET_FIFO("false"),
    .CDC_SYNC_STAGES(3), .TDATA_WIDTH(512), .TUSER_WIDTH(72), .TID_WIDTH(1), .TDEST_WIDTH(1),
    .USE_ADV_FEATURES("0000"), .ECC_MODE("no_ecc"), .SIM_ASSERT_CHK(0)
  ) inq (
    .s_aclk(ap_clk), .s_aresetn(fifo_rst_n), .m_aclk(clk200),
    .s_axis_tvalid(s_axis_in_tvalid), .s_axis_tready(s_axis_in_tready), .s_axis_tdata(s_axis_in_tdata),
    .s_axis_tkeep(s_axis_in_tkeep), .s_axis_tstrb(s_axis_in_tkeep), .s_axis_tlast(s_axis_in_tlast),
    .s_axis_tuser(s_axis_in_tuser), .s_axis_tid(1'b0), .s_axis_tdest(1'b0),
    .m_axis_tvalid(in_vld), .m_axis_tready(in_rdy), .m_axis_tdata(in_data), .m_axis_tkeep(in_keep), .m_axis_tstrb(),
    .m_axis_tlast(in_last), .m_axis_tuser(in_user), .m_axis_tid(), .m_axis_tdest(),
    .wr_data_count_axis(), .rd_data_count_axis(), .prog_full_axis(), .prog_empty_axis(),
    .almost_full_axis(), .almost_empty_axis(), .injectsbiterr_axis(1'b0), .injectdbiterr_axis(1'b0),
    .sbiterr_axis(), .dbiterr_axis()
  );

  // ---------------------------------------------------------------- CSRs: ap_clk <-> clk200
  wire        c_awvld, c_awrdy, c_wvld, c_wrdy, c_bvld, c_brdy, c_arvld, c_arrdy, c_rvld, c_rrdy;
  wire [15:0] c_awaddr, c_araddr;
  wire [31:0] c_wdata, c_rdata;
  wire [3:0]  c_wstrb;
  wire [1:0]  c_bresp, c_rresp;
  axil_cdc cdc (
    .s_clk(ap_clk), .s_rst(~fifo_rst_n), .m_clk(clk200), .m_rst(afu_rst),
    .s_awaddr(s_axi_control_AWADDR), .s_awvalid(s_axi_control_AWVALID), .s_awready(s_axi_control_AWREADY),
    .s_wdata(s_axi_control_WDATA), .s_wstrb(s_axi_control_WSTRB), .s_wvalid(s_axi_control_WVALID), .s_wready(s_axi_control_WREADY),
    .s_bresp(s_axi_control_BRESP), .s_bvalid(s_axi_control_BVALID), .s_bready(s_axi_control_BREADY),
    .s_araddr(s_axi_control_ARADDR), .s_arvalid(s_axi_control_ARVALID), .s_arready(s_axi_control_ARREADY),
    .s_rdata(s_axi_control_RDATA), .s_rresp(s_axi_control_RRESP), .s_rvalid(s_axi_control_RVALID), .s_rready(s_axi_control_RREADY),
    .m_awaddr(c_awaddr), .m_awvalid(c_awvld), .m_awready(c_awrdy),
    .m_wdata(c_wdata), .m_wstrb(c_wstrb), .m_wvalid(c_wvld), .m_wready(c_wrdy),
    .m_bresp(c_bresp), .m_bvalid(c_bvld), .m_bready(c_brdy),
    .m_araddr(c_araddr), .m_arvalid(c_arvld), .m_arready(c_arrdy),
    .m_rdata(c_rdata), .m_rresp(c_rresp), .m_rvalid(c_rvld), .m_rready(c_rrdy)
  );

  // ---------------------------------------------------------------- the AFU at 200 MHz
  wire         tx_vld, tx_rdy, tx_last;
  wire [511:0] tx_data;
  wire [63:0]  tx_keep;
  wire [71:0]  tx_user;
  zuc_afu afu (
    .clk(clk200), .reset(afu_rst), .afu_soft_reset(),
    .pci2sbu_axi4stream_rdy(in_rdy), .pci2sbu_axi4stream_vld(in_vld), .pci2sbu_axi4stream_tdata(in_data),
    .pci2sbu_axi4stream_tkeep(in_keep), .pci2sbu_axi4stream_tlast(in_last), .pci2sbu_axi4stream_tuser(in_user),
    .sbu2pci_axi4stream_rdy(tx_rdy), .sbu2pci_axi4stream_vld(tx_vld), .sbu2pci_axi4stream_tdata(tx_data),
    .sbu2pci_axi4stream_tkeep(tx_keep), .sbu2pci_axi4stream_tlast(tx_last), .sbu2pci_axi4stream_tuser(tx_user),
    .axilite_aw_rdy(c_awrdy), .axilite_aw_vld(c_awvld), .axilite_aw_addr({48'b0, c_awaddr}), .axilite_aw_prot(3'b0),
    .axilite_w_rdy(c_wrdy), .axilite_w_vld(c_wvld), .axilite_w_data(c_wdata), .axilite_w_strobe(c_wstrb),
    .axilite_b_rdy(c_brdy), .axilite_b_vld(c_bvld), .axilite_b_resp(c_bresp),
    .axilite_ar_rdy(c_arrdy), .axilite_ar_vld(c_arvld), .axilite_ar_addr({48'b0, c_araddr}), .axilite_ar_prot(3'b0),
    .axilite_r_rdy(c_rrdy), .axilite_r_vld(c_rvld), .axilite_r_data(c_rdata), .axilite_r_resp(c_rresp),
    .fld_pci_sample_soft_reset(), .fld_pci_sample_enable(), .afu_events()
  );

  // ---------------------------------------------------------------- response stream: clk200 -> ap_clk, the safe sink
  xpm_fifo_axis #(
    .CLOCKING_MODE("independent_clock"), .FIFO_DEPTH(512), .FIFO_MEMORY_TYPE("block"), .PACKET_FIFO("false"),
    .CDC_SYNC_STAGES(3), .TDATA_WIDTH(512), .TUSER_WIDTH(72), .TID_WIDTH(1), .TDEST_WIDTH(1),
    .USE_ADV_FEATURES("0000"), .ECC_MODE("no_ecc"), .SIM_ASSERT_CHK(0)
  ) outq (
    .s_aclk(clk200), .s_aresetn(~afu_rst), .m_aclk(ap_clk),
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

// AXI4-Lite clock crossing: one small async FIFO per channel (AW, W, AR forward; B, R back). Each FIFO's write side
// ready is !full; its read side is first-word-fall-through, valid = !empty.
module axil_cdc (
  input  wire s_clk, input wire s_rst, input wire m_clk, input wire m_rst,
  input  wire [15:0] s_awaddr, input wire s_awvalid, output wire s_awready,
  input  wire [31:0] s_wdata, input wire [3:0] s_wstrb, input wire s_wvalid, output wire s_wready,
  output wire [1:0] s_bresp, output wire s_bvalid, input wire s_bready,
  input  wire [15:0] s_araddr, input wire s_arvalid, output wire s_arready,
  output wire [31:0] s_rdata, output wire [1:0] s_rresp, output wire s_rvalid, input wire s_rready,
  output wire [15:0] m_awaddr, output wire m_awvalid, input wire m_awready,
  output wire [31:0] m_wdata, output wire [3:0] m_wstrb, output wire m_wvalid, input wire m_wready,
  input  wire [1:0] m_bresp, input wire m_bvalid, output wire m_bready,
  output wire [15:0] m_araddr, output wire m_arvalid, input wire m_arready,
  input  wire [31:0] m_rdata, input wire [1:0] m_rresp, input wire m_rvalid, output wire m_rready
);
  cdc_ch #(16) aw (.wclk(s_clk), .wrst(s_rst), .din(s_awaddr), .wvld(s_awvalid), .wrdy(s_awready),
                   .rclk(m_clk), .dout(m_awaddr), .rvld(m_awvalid), .rrdy(m_awready));
  cdc_ch #(36) w  (.wclk(s_clk), .wrst(s_rst), .din({s_wstrb, s_wdata}), .wvld(s_wvalid), .wrdy(s_wready),
                   .rclk(m_clk), .dout({m_wstrb, m_wdata}), .rvld(m_wvalid), .rrdy(m_wready));
  cdc_ch #(16) ar (.wclk(s_clk), .wrst(s_rst), .din(s_araddr), .wvld(s_arvalid), .wrdy(s_arready),
                   .rclk(m_clk), .dout(m_araddr), .rvld(m_arvalid), .rrdy(m_arready));
  cdc_ch #(2)  b  (.wclk(m_clk), .wrst(m_rst), .din(m_bresp), .wvld(m_bvalid), .wrdy(m_bready),
                   .rclk(s_clk), .dout(s_bresp), .rvld(s_bvalid), .rrdy(s_bready));
  cdc_ch #(34) r  (.wclk(m_clk), .wrst(m_rst), .din({m_rresp, m_rdata}), .wvld(m_rvalid), .wrdy(m_rready),
                   .rclk(s_clk), .dout({s_rresp, s_rdata}), .rvld(s_rvalid), .rrdy(s_rready));
endmodule

module cdc_ch #(parameter W = 32) (
  input wire wclk, input wire wrst, input wire [W-1:0] din, input wire wvld, output wire wrdy,
  input wire rclk, output wire [W-1:0] dout, output wire rvld, input wire rrdy
);
  wire full, empty, wbusy, rbusy;
  assign wrdy = !full && !wbusy;
  assign rvld = !empty && !rbusy;
  xpm_fifo_async #(
    .FIFO_MEMORY_TYPE("distributed"), .FIFO_WRITE_DEPTH(16), .WRITE_DATA_WIDTH(W), .READ_DATA_WIDTH(W),
    .READ_MODE("fwft"), .FIFO_READ_LATENCY(0), .CDC_SYNC_STAGES(3), .USE_ADV_FEATURES("0000"),
    .ECC_MODE("no_ecc"), .SIM_ASSERT_CHK(0)
  ) f (
    .rst(wrst), .wr_clk(wclk), .wr_en(wvld && wrdy), .din(din), .full(full), .wr_rst_busy(wbusy),
    .rd_clk(rclk), .rd_en(rvld && rrdy), .dout(dout), .empty(empty), .rd_rst_busy(rbusy),
    .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
    .almost_full(), .almost_empty(), .prog_full(), .prog_empty(), .data_valid(), .overflow(), .underflow(),
    .wr_ack(), .wr_data_count(), .rd_data_count(), .sbiterr(), .dbiterr()
  );
endmodule
