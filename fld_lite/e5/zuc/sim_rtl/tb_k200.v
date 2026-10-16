`timescale 1ns/1ps
module tb_k;
  integer fi, fo, fc, rc, bp, seed, nin, nout, i; reg [511:0] d; reg l; reg [71:0] u; reg [19:0] ca; reg [31:0] cd;
  reg clk = 0; always #2.0 clk = ~clk;   // 250 MHz ap_clk
  reg reset = 1;
  reg          in_vld = 0; wire in_rdy; reg [511:0] in_data = 0; reg [63:0] in_keep = 0; reg in_last = 0; reg [71:0] in_user = 0;
  wire out_vld; wire out_rdy; reg rdy_rand = 1; integer fifosink = 0; reg [3:0] scnt = 0;
  assign out_rdy = fifosink ? (scnt < 4) : rdy_rand; wire [511:0] out_data; wire [63:0] out_keep; wire out_last; wire [71:0] out_user;
  wire aw_rdy, w_rdy, b_vld, ar_rdy, r_vld; wire [1:0] b_resp, r_resp; wire [31:0] r_data;
  reg aw_vld=0, w_vld=0, ar_vld=0; reg [63:0] aw_addr=0, ar_addr=0; reg [31:0] w_data=0;
  wire afu_soft_reset, fld_rst; wire [1:0] fld_en; wire [63:0] ev;
  // E5.2: the Vitis RTL kernel wrapper (zuc_k: synthesizable IP + safe output FIFO) instead of the bare AFU
  zuc_k dut(.ap_clk(clk), .ap_rst_n(~reset),
    .s_axi_control_AWADDR(aw_addr[15:0]), .s_axi_control_AWVALID(aw_vld), .s_axi_control_AWREADY(aw_rdy),
    .s_axi_control_WDATA(w_data), .s_axi_control_WSTRB(4'hf), .s_axi_control_WVALID(w_vld), .s_axi_control_WREADY(w_rdy),
    .s_axi_control_BRESP(b_resp), .s_axi_control_BVALID(b_vld), .s_axi_control_BREADY(1'b1),
    .s_axi_control_ARADDR(ar_addr[15:0]), .s_axi_control_ARVALID(ar_vld), .s_axi_control_ARREADY(ar_rdy),
    .s_axi_control_RDATA(r_data), .s_axi_control_RRESP(r_resp), .s_axi_control_RVALID(r_vld), .s_axi_control_RREADY(1'b1),
    .s_axis_in_tdata(in_data), .s_axis_in_tkeep(in_keep), .s_axis_in_tuser(in_user), .s_axis_in_tlast(in_last),
    .s_axis_in_tvalid(in_vld), .s_axis_in_tready(in_rdy),
    .m_axis_out_tdata(out_data), .m_axis_out_tkeep(out_keep), .m_axis_out_tuser(out_user), .m_axis_out_tlast(out_last),
    .m_axis_out_tvalid(out_vld), .m_axis_out_tready(out_rdy));

  task csr_wr(input [19:0] a, input [31:0] d); begin
    @(posedge clk); aw_addr <= a; aw_vld <= 1;
    @(posedge clk); while (!aw_rdy) @(posedge clk); aw_vld <= 0; w_data <= d; w_vld <= 1;
    @(posedge clk); while (!w_rdy) @(posedge clk); w_vld <= 0;
    while (!b_vld) @(posedge clk);
    $fdisplay(fo, "CSRW %05x %08x", a, d); end endtask
  task csr_rd(input [19:0] a); begin
    @(posedge clk); ar_addr <= a; ar_vld <= 1;
    @(posedge clk); while (!ar_rdy) @(posedge clk); ar_vld <= 0;
    while (!r_vld) @(posedge clk);
    $fdisplay(fo, "CSRR %05x %08x", a, r_data); end endtask

  reg [8*8-1:0] op;
  // output monitor: log every accepted beat
  always @(posedge clk) if (!reset && out_vld && out_rdy) begin
    $fdisplay(fo, "OUT %0128x %016x %0d %018x", out_data, out_keep, out_last, out_user); nout = nout + 1; end
  // count cycles where valid was high while rdy low (predictive-ready check)
  integer vld_while_notrdy = 0;
  integer notrdy = 0;
  always @(posedge clk) if (!reset && out_vld && !out_rdy) vld_while_notrdy = vld_while_notrdy + 1;
  always @(posedge clk) if (!reset && !out_rdy) notrdy = notrdy + 1;
  // TX backpressure
  always @(posedge clk) if (bp > 0) rdy_rand <= (({$random(seed)} % 100) >= bp); else rdy_rand <= 1;
  // occupancy-driven sink: 4-deep FIFO, tready = count<4, random drain
  always @(posedge clk) if (reset) scnt <= 0; else if (fifosink) scnt <= scnt + (out_vld && out_rdy) - ((scnt != 0) && (({$random(seed)} % 100) >= bp));
  integer cyc = 0;
  always @(posedge clk) begin
    cyc = cyc + 1;
    if (cyc % 20000 == 0) $display("cyc=%0d nin=%0d nout=%0d in_st=%0d msgout_st=%0d sbu_st=%0d scnt=%0d rdy=%b vld=%b ctr1=%h", cyc, nin, nout,
        dut.afu.packet_in_nstate, dut.afu.message_out_nstate, dut.afu.sbu2pci_out_nstate, scnt, out_rdy, out_vld, dut.afu.afu_counter1);
    if (cyc == 1500000) begin $display("WATCHDOG: stopping"); $fdisplay(fo, "WATCHDOG nin=%0d nout=%0d", nin, nout); $fclose(fo); $finish; end
  end
  initial begin
    nout = 0; seed = 1;
    if (!$value$plusargs("bp=%d", bp)) bp = 0;
    if ($test$plusargs("fifosink")) fifosink = 1;
    fo = $fopen("out.txt", "w");
    repeat (40) @(posedge clk); reset <= 0; repeat (100) @(posedge clk);
    fc = $fopen("csr.txt", "r");
    if (fc != 0) begin
      while (!$feof(fc)) begin
        rc = $fscanf(fc, "%s %h %h\n", op, ca, cd);
        if (rc == 3 && op == "W") csr_wr(ca, cd);
        else if (rc >= 2 && op == "R") csr_rd(ca);
        else if (rc >= 2 && op == "S") repeat (ca) @(posedge clk);
      end
    end
    fi = $fopen("in.txt", "r"); nin = 0;
    while (!$feof(fi)) begin
      rc = $fscanf(fi, "%h %h %h\n", d, l, u);
      if (rc == 3) begin
        in_data <= d; in_last <= l; in_user <= u; in_keep <= 64'hffffffffffffffff; in_vld <= 1;
        @(posedge clk); while (!in_rdy) @(posedge clk);
        in_vld <= 0; in_data <= {16{32'hdeadbeef}}; in_user <= 72'h0; nin = nin + 1;
      end
    end
    repeat (20000) @(posedge clk);
    // post-run CSR reads
    csr_rd(20'h01030); csr_rd(20'h01100); csr_rd(20'h01104); csr_rd(20'h01108); csr_rd(20'h0110c);
    csr_rd(20'h01118); csr_rd(20'h0111c); csr_rd(20'h01120); csr_rd(20'h01124); csr_rd(20'h01200); csr_rd(20'h01234);
    $fdisplay(fo, "DONE in_beats=%0d out_beats=%0d vld_while_notrdy=%0d rdy_low_cycles=%0d", nin, nout, vld_while_notrdy, notrdy);
    $fclose(fo); $finish;
  end
endmodule
