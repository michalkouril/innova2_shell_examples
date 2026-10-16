// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// E5.2: synthesizable replacements for the eight Vivado 2019.2 IP cores inside the FlexDriver ZUC AFU, same module
// names and ports (work/docs/zuc_afu_port_spec.md 9.1). Behaviour as in work/zuc_spec_sim/ipmodels.v, except:
//   axis_512x512b_fifo  -> xpm_fifo_axis, PACKET mode, depth 1024 (the AFU plans with 514 entries and does not re-check
//                          tready per beat, so >= 515 real entries are required; 1024 gives margin)
//   sbox_s0 / sbox_s1   -> combinational case ROMs generated from s0_box.coe / s1_box.coe

module sbox_s0(input [7:0] a, output reg [7:0] spo);
  always @(*) case (a)
    8'h00: spo = 8'h3e;
    8'h01: spo = 8'h72;
    8'h02: spo = 8'h5b;
    8'h03: spo = 8'h47;
    8'h04: spo = 8'hca;
    8'h05: spo = 8'he0;
    8'h06: spo = 8'h00;
    8'h07: spo = 8'h33;
    8'h08: spo = 8'h04;
    8'h09: spo = 8'hd1;
    8'h0a: spo = 8'h54;
    8'h0b: spo = 8'h98;
    8'h0c: spo = 8'h09;
    8'h0d: spo = 8'hb9;
    8'h0e: spo = 8'h6d;
    8'h0f: spo = 8'hcb;
    8'h10: spo = 8'h7b;
    8'h11: spo = 8'h1b;
    8'h12: spo = 8'hf9;
    8'h13: spo = 8'h32;
    8'h14: spo = 8'haf;
    8'h15: spo = 8'h9d;
    8'h16: spo = 8'h6a;
    8'h17: spo = 8'ha5;
    8'h18: spo = 8'hb8;
    8'h19: spo = 8'h2d;
    8'h1a: spo = 8'hfc;
    8'h1b: spo = 8'h1d;
    8'h1c: spo = 8'h08;
    8'h1d: spo = 8'h53;
    8'h1e: spo = 8'h03;
    8'h1f: spo = 8'h90;
    8'h20: spo = 8'h4d;
    8'h21: spo = 8'h4e;
    8'h22: spo = 8'h84;
    8'h23: spo = 8'h99;
    8'h24: spo = 8'he4;
    8'h25: spo = 8'hce;
    8'h26: spo = 8'hd9;
    8'h27: spo = 8'h91;
    8'h28: spo = 8'hdd;
    8'h29: spo = 8'hb6;
    8'h2a: spo = 8'h85;
    8'h2b: spo = 8'h48;
    8'h2c: spo = 8'h8b;
    8'h2d: spo = 8'h29;
    8'h2e: spo = 8'h6e;
    8'h2f: spo = 8'hac;
    8'h30: spo = 8'hcd;
    8'h31: spo = 8'hc1;
    8'h32: spo = 8'hf8;
    8'h33: spo = 8'h1e;
    8'h34: spo = 8'h73;
    8'h35: spo = 8'h43;
    8'h36: spo = 8'h69;
    8'h37: spo = 8'hc6;
    8'h38: spo = 8'hb5;
    8'h39: spo = 8'hbd;
    8'h3a: spo = 8'hfd;
    8'h3b: spo = 8'h39;
    8'h3c: spo = 8'h63;
    8'h3d: spo = 8'h20;
    8'h3e: spo = 8'hd4;
    8'h3f: spo = 8'h38;
    8'h40: spo = 8'h76;
    8'h41: spo = 8'h7d;
    8'h42: spo = 8'hb2;
    8'h43: spo = 8'ha7;
    8'h44: spo = 8'hcf;
    8'h45: spo = 8'hed;
    8'h46: spo = 8'h57;
    8'h47: spo = 8'hc5;
    8'h48: spo = 8'hf3;
    8'h49: spo = 8'h2c;
    8'h4a: spo = 8'hbb;
    8'h4b: spo = 8'h14;
    8'h4c: spo = 8'h21;
    8'h4d: spo = 8'h06;
    8'h4e: spo = 8'h55;
    8'h4f: spo = 8'h9b;
    8'h50: spo = 8'he3;
    8'h51: spo = 8'hef;
    8'h52: spo = 8'h5e;
    8'h53: spo = 8'h31;
    8'h54: spo = 8'h4f;
    8'h55: spo = 8'h7f;
    8'h56: spo = 8'h5a;
    8'h57: spo = 8'ha4;
    8'h58: spo = 8'h0d;
    8'h59: spo = 8'h82;
    8'h5a: spo = 8'h51;
    8'h5b: spo = 8'h49;
    8'h5c: spo = 8'h5f;
    8'h5d: spo = 8'hba;
    8'h5e: spo = 8'h58;
    8'h5f: spo = 8'h1c;
    8'h60: spo = 8'h4a;
    8'h61: spo = 8'h16;
    8'h62: spo = 8'hd5;
    8'h63: spo = 8'h17;
    8'h64: spo = 8'ha8;
    8'h65: spo = 8'h92;
    8'h66: spo = 8'h24;
    8'h67: spo = 8'h1f;
    8'h68: spo = 8'h8c;
    8'h69: spo = 8'hff;
    8'h6a: spo = 8'hd8;
    8'h6b: spo = 8'hae;
    8'h6c: spo = 8'h2e;
    8'h6d: spo = 8'h01;
    8'h6e: spo = 8'hd3;
    8'h6f: spo = 8'had;
    8'h70: spo = 8'h3b;
    8'h71: spo = 8'h4b;
    8'h72: spo = 8'hda;
    8'h73: spo = 8'h46;
    8'h74: spo = 8'heb;
    8'h75: spo = 8'hc9;
    8'h76: spo = 8'hde;
    8'h77: spo = 8'h9a;
    8'h78: spo = 8'h8f;
    8'h79: spo = 8'h87;
    8'h7a: spo = 8'hd7;
    8'h7b: spo = 8'h3a;
    8'h7c: spo = 8'h80;
    8'h7d: spo = 8'h6f;
    8'h7e: spo = 8'h2f;
    8'h7f: spo = 8'hc8;
    8'h80: spo = 8'hb1;
    8'h81: spo = 8'hb4;
    8'h82: spo = 8'h37;
    8'h83: spo = 8'hf7;
    8'h84: spo = 8'h0a;
    8'h85: spo = 8'h22;
    8'h86: spo = 8'h13;
    8'h87: spo = 8'h28;
    8'h88: spo = 8'h7c;
    8'h89: spo = 8'hcc;
    8'h8a: spo = 8'h3c;
    8'h8b: spo = 8'h89;
    8'h8c: spo = 8'hc7;
    8'h8d: spo = 8'hc3;
    8'h8e: spo = 8'h96;
    8'h8f: spo = 8'h56;
    8'h90: spo = 8'h07;
    8'h91: spo = 8'hbf;
    8'h92: spo = 8'h7e;
    8'h93: spo = 8'hf0;
    8'h94: spo = 8'h0b;
    8'h95: spo = 8'h2b;
    8'h96: spo = 8'h97;
    8'h97: spo = 8'h52;
    8'h98: spo = 8'h35;
    8'h99: spo = 8'h41;
    8'h9a: spo = 8'h79;
    8'h9b: spo = 8'h61;
    8'h9c: spo = 8'ha6;
    8'h9d: spo = 8'h4c;
    8'h9e: spo = 8'h10;
    8'h9f: spo = 8'hfe;
    8'ha0: spo = 8'hbc;
    8'ha1: spo = 8'h26;
    8'ha2: spo = 8'h95;
    8'ha3: spo = 8'h88;
    8'ha4: spo = 8'h8a;
    8'ha5: spo = 8'hb0;
    8'ha6: spo = 8'ha3;
    8'ha7: spo = 8'hfb;
    8'ha8: spo = 8'hc0;
    8'ha9: spo = 8'h18;
    8'haa: spo = 8'h94;
    8'hab: spo = 8'hf2;
    8'hac: spo = 8'he1;
    8'had: spo = 8'he5;
    8'hae: spo = 8'he9;
    8'haf: spo = 8'h5d;
    8'hb0: spo = 8'hd0;
    8'hb1: spo = 8'hdc;
    8'hb2: spo = 8'h11;
    8'hb3: spo = 8'h66;
    8'hb4: spo = 8'h64;
    8'hb5: spo = 8'h5c;
    8'hb6: spo = 8'hec;
    8'hb7: spo = 8'h59;
    8'hb8: spo = 8'h42;
    8'hb9: spo = 8'h75;
    8'hba: spo = 8'h12;
    8'hbb: spo = 8'hf5;
    8'hbc: spo = 8'h74;
    8'hbd: spo = 8'h9c;
    8'hbe: spo = 8'haa;
    8'hbf: spo = 8'h23;
    8'hc0: spo = 8'h0e;
    8'hc1: spo = 8'h86;
    8'hc2: spo = 8'hab;
    8'hc3: spo = 8'hbe;
    8'hc4: spo = 8'h2a;
    8'hc5: spo = 8'h02;
    8'hc6: spo = 8'he7;
    8'hc7: spo = 8'h67;
    8'hc8: spo = 8'he6;
    8'hc9: spo = 8'h44;
    8'hca: spo = 8'ha2;
    8'hcb: spo = 8'h6c;
    8'hcc: spo = 8'hc2;
    8'hcd: spo = 8'h93;
    8'hce: spo = 8'h9f;
    8'hcf: spo = 8'hf1;
    8'hd0: spo = 8'hf6;
    8'hd1: spo = 8'hfa;
    8'hd2: spo = 8'h36;
    8'hd3: spo = 8'hd2;
    8'hd4: spo = 8'h50;
    8'hd5: spo = 8'h68;
    8'hd6: spo = 8'h9e;
    8'hd7: spo = 8'h62;
    8'hd8: spo = 8'h71;
    8'hd9: spo = 8'h15;
    8'hda: spo = 8'h3d;
    8'hdb: spo = 8'hd6;
    8'hdc: spo = 8'h40;
    8'hdd: spo = 8'hc4;
    8'hde: spo = 8'he2;
    8'hdf: spo = 8'h0f;
    8'he0: spo = 8'h8e;
    8'he1: spo = 8'h83;
    8'he2: spo = 8'h77;
    8'he3: spo = 8'h6b;
    8'he4: spo = 8'h25;
    8'he5: spo = 8'h05;
    8'he6: spo = 8'h3f;
    8'he7: spo = 8'h0c;
    8'he8: spo = 8'h30;
    8'he9: spo = 8'hea;
    8'hea: spo = 8'h70;
    8'heb: spo = 8'hb7;
    8'hec: spo = 8'ha1;
    8'hed: spo = 8'he8;
    8'hee: spo = 8'ha9;
    8'hef: spo = 8'h65;
    8'hf0: spo = 8'h8d;
    8'hf1: spo = 8'h27;
    8'hf2: spo = 8'h1a;
    8'hf3: spo = 8'hdb;
    8'hf4: spo = 8'h81;
    8'hf5: spo = 8'hb3;
    8'hf6: spo = 8'ha0;
    8'hf7: spo = 8'hf4;
    8'hf8: spo = 8'h45;
    8'hf9: spo = 8'h7a;
    8'hfa: spo = 8'h19;
    8'hfb: spo = 8'hdf;
    8'hfc: spo = 8'hee;
    8'hfd: spo = 8'h78;
    8'hfe: spo = 8'h34;
    8'hff: spo = 8'h60;
  endcase
endmodule

module sbox_s1(input [7:0] a, output reg [7:0] spo);
  always @(*) case (a)
    8'h00: spo = 8'h55;
    8'h01: spo = 8'hc2;
    8'h02: spo = 8'h63;
    8'h03: spo = 8'h71;
    8'h04: spo = 8'h3b;
    8'h05: spo = 8'hc8;
    8'h06: spo = 8'h47;
    8'h07: spo = 8'h86;
    8'h08: spo = 8'h9f;
    8'h09: spo = 8'h3c;
    8'h0a: spo = 8'hda;
    8'h0b: spo = 8'h5b;
    8'h0c: spo = 8'h29;
    8'h0d: spo = 8'haa;
    8'h0e: spo = 8'hfd;
    8'h0f: spo = 8'h77;
    8'h10: spo = 8'h8c;
    8'h11: spo = 8'hc5;
    8'h12: spo = 8'h94;
    8'h13: spo = 8'h0c;
    8'h14: spo = 8'ha6;
    8'h15: spo = 8'h1a;
    8'h16: spo = 8'h13;
    8'h17: spo = 8'h00;
    8'h18: spo = 8'he3;
    8'h19: spo = 8'ha8;
    8'h1a: spo = 8'h16;
    8'h1b: spo = 8'h72;
    8'h1c: spo = 8'h40;
    8'h1d: spo = 8'hf9;
    8'h1e: spo = 8'hf8;
    8'h1f: spo = 8'h42;
    8'h20: spo = 8'h44;
    8'h21: spo = 8'h26;
    8'h22: spo = 8'h68;
    8'h23: spo = 8'h96;
    8'h24: spo = 8'h81;
    8'h25: spo = 8'hd9;
    8'h26: spo = 8'h45;
    8'h27: spo = 8'h3e;
    8'h28: spo = 8'h10;
    8'h29: spo = 8'h76;
    8'h2a: spo = 8'hc6;
    8'h2b: spo = 8'ha7;
    8'h2c: spo = 8'h8b;
    8'h2d: spo = 8'h39;
    8'h2e: spo = 8'h43;
    8'h2f: spo = 8'he1;
    8'h30: spo = 8'h3a;
    8'h31: spo = 8'hb5;
    8'h32: spo = 8'h56;
    8'h33: spo = 8'h2a;
    8'h34: spo = 8'hc0;
    8'h35: spo = 8'h6d;
    8'h36: spo = 8'hb3;
    8'h37: spo = 8'h05;
    8'h38: spo = 8'h22;
    8'h39: spo = 8'h66;
    8'h3a: spo = 8'hbf;
    8'h3b: spo = 8'hdc;
    8'h3c: spo = 8'h0b;
    8'h3d: spo = 8'hfa;
    8'h3e: spo = 8'h62;
    8'h3f: spo = 8'h48;
    8'h40: spo = 8'hdd;
    8'h41: spo = 8'h20;
    8'h42: spo = 8'h11;
    8'h43: spo = 8'h06;
    8'h44: spo = 8'h36;
    8'h45: spo = 8'hc9;
    8'h46: spo = 8'hc1;
    8'h47: spo = 8'hcf;
    8'h48: spo = 8'hf6;
    8'h49: spo = 8'h27;
    8'h4a: spo = 8'h52;
    8'h4b: spo = 8'hbb;
    8'h4c: spo = 8'h69;
    8'h4d: spo = 8'hf5;
    8'h4e: spo = 8'hd4;
    8'h4f: spo = 8'h87;
    8'h50: spo = 8'h7f;
    8'h51: spo = 8'h84;
    8'h52: spo = 8'h4c;
    8'h53: spo = 8'hd2;
    8'h54: spo = 8'h9c;
    8'h55: spo = 8'h57;
    8'h56: spo = 8'ha4;
    8'h57: spo = 8'hbc;
    8'h58: spo = 8'h4f;
    8'h59: spo = 8'h9a;
    8'h5a: spo = 8'hdf;
    8'h5b: spo = 8'hfe;
    8'h5c: spo = 8'hd6;
    8'h5d: spo = 8'h8d;
    8'h5e: spo = 8'h7a;
    8'h5f: spo = 8'heb;
    8'h60: spo = 8'h2b;
    8'h61: spo = 8'h53;
    8'h62: spo = 8'hd8;
    8'h63: spo = 8'h5c;
    8'h64: spo = 8'ha1;
    8'h65: spo = 8'h14;
    8'h66: spo = 8'h17;
    8'h67: spo = 8'hfb;
    8'h68: spo = 8'h23;
    8'h69: spo = 8'hd5;
    8'h6a: spo = 8'h7d;
    8'h6b: spo = 8'h30;
    8'h6c: spo = 8'h67;
    8'h6d: spo = 8'h73;
    8'h6e: spo = 8'h08;
    8'h6f: spo = 8'h09;
    8'h70: spo = 8'hee;
    8'h71: spo = 8'hb7;
    8'h72: spo = 8'h70;
    8'h73: spo = 8'h3f;
    8'h74: spo = 8'h61;
    8'h75: spo = 8'hb2;
    8'h76: spo = 8'h19;
    8'h77: spo = 8'h8e;
    8'h78: spo = 8'h4e;
    8'h79: spo = 8'he5;
    8'h7a: spo = 8'h4b;
    8'h7b: spo = 8'h93;
    8'h7c: spo = 8'h8f;
    8'h7d: spo = 8'h5d;
    8'h7e: spo = 8'hdb;
    8'h7f: spo = 8'ha9;
    8'h80: spo = 8'had;
    8'h81: spo = 8'hf1;
    8'h82: spo = 8'hae;
    8'h83: spo = 8'h2e;
    8'h84: spo = 8'hcb;
    8'h85: spo = 8'h0d;
    8'h86: spo = 8'hfc;
    8'h87: spo = 8'hf4;
    8'h88: spo = 8'h2d;
    8'h89: spo = 8'h46;
    8'h8a: spo = 8'h6e;
    8'h8b: spo = 8'h1d;
    8'h8c: spo = 8'h97;
    8'h8d: spo = 8'he8;
    8'h8e: spo = 8'hd1;
    8'h8f: spo = 8'he9;
    8'h90: spo = 8'h4d;
    8'h91: spo = 8'h37;
    8'h92: spo = 8'ha5;
    8'h93: spo = 8'h75;
    8'h94: spo = 8'h5e;
    8'h95: spo = 8'h83;
    8'h96: spo = 8'h9e;
    8'h97: spo = 8'hab;
    8'h98: spo = 8'h82;
    8'h99: spo = 8'h9d;
    8'h9a: spo = 8'hb9;
    8'h9b: spo = 8'h1c;
    8'h9c: spo = 8'he0;
    8'h9d: spo = 8'hcd;
    8'h9e: spo = 8'h49;
    8'h9f: spo = 8'h89;
    8'ha0: spo = 8'h01;
    8'ha1: spo = 8'hb6;
    8'ha2: spo = 8'hbd;
    8'ha3: spo = 8'h58;
    8'ha4: spo = 8'h24;
    8'ha5: spo = 8'ha2;
    8'ha6: spo = 8'h5f;
    8'ha7: spo = 8'h38;
    8'ha8: spo = 8'h78;
    8'ha9: spo = 8'h99;
    8'haa: spo = 8'h15;
    8'hab: spo = 8'h90;
    8'hac: spo = 8'h50;
    8'had: spo = 8'hb8;
    8'hae: spo = 8'h95;
    8'haf: spo = 8'he4;
    8'hb0: spo = 8'hd0;
    8'hb1: spo = 8'h91;
    8'hb2: spo = 8'hc7;
    8'hb3: spo = 8'hce;
    8'hb4: spo = 8'hed;
    8'hb5: spo = 8'h0f;
    8'hb6: spo = 8'hb4;
    8'hb7: spo = 8'h6f;
    8'hb8: spo = 8'ha0;
    8'hb9: spo = 8'hcc;
    8'hba: spo = 8'hf0;
    8'hbb: spo = 8'h02;
    8'hbc: spo = 8'h4a;
    8'hbd: spo = 8'h79;
    8'hbe: spo = 8'hc3;
    8'hbf: spo = 8'hde;
    8'hc0: spo = 8'ha3;
    8'hc1: spo = 8'hef;
    8'hc2: spo = 8'hea;
    8'hc3: spo = 8'h51;
    8'hc4: spo = 8'he6;
    8'hc5: spo = 8'h6b;
    8'hc6: spo = 8'h18;
    8'hc7: spo = 8'hec;
    8'hc8: spo = 8'h1b;
    8'hc9: spo = 8'h2c;
    8'hca: spo = 8'h80;
    8'hcb: spo = 8'hf7;
    8'hcc: spo = 8'h74;
    8'hcd: spo = 8'he7;
    8'hce: spo = 8'hff;
    8'hcf: spo = 8'h21;
    8'hd0: spo = 8'h5a;
    8'hd1: spo = 8'h6a;
    8'hd2: spo = 8'h54;
    8'hd3: spo = 8'h1e;
    8'hd4: spo = 8'h41;
    8'hd5: spo = 8'h31;
    8'hd6: spo = 8'h92;
    8'hd7: spo = 8'h35;
    8'hd8: spo = 8'hc4;
    8'hd9: spo = 8'h33;
    8'hda: spo = 8'h07;
    8'hdb: spo = 8'h0a;
    8'hdc: spo = 8'hba;
    8'hdd: spo = 8'h7e;
    8'hde: spo = 8'h0e;
    8'hdf: spo = 8'h34;
    8'he0: spo = 8'h88;
    8'he1: spo = 8'hb1;
    8'he2: spo = 8'h98;
    8'he3: spo = 8'h7c;
    8'he4: spo = 8'hf3;
    8'he5: spo = 8'h3d;
    8'he6: spo = 8'h60;
    8'he7: spo = 8'h6c;
    8'he8: spo = 8'h7b;
    8'he9: spo = 8'hca;
    8'hea: spo = 8'hd3;
    8'heb: spo = 8'h1f;
    8'hec: spo = 8'h32;
    8'hed: spo = 8'h65;
    8'hee: spo = 8'h04;
    8'hef: spo = 8'h28;
    8'hf0: spo = 8'h64;
    8'hf1: spo = 8'hbe;
    8'hf2: spo = 8'h85;
    8'hf3: spo = 8'h9b;
    8'hf4: spo = 8'h2f;
    8'hf5: spo = 8'h59;
    8'hf6: spo = 8'h8a;
    8'hf7: spo = 8'hd7;
    8'hf8: spo = 8'hb0;
    8'hf9: spo = 8'h25;
    8'hfa: spo = 8'hac;
    8'hfb: spo = 8'haf;
    8'hfc: spo = 8'h12;
    8'hfd: spo = 8'h03;
    8'hfe: spo = 8'he2;
    8'hff: spo = 8'hf2;
  endcase
endmodule

// blk_mem_gen SDP, no output registers => read latency 1
module blk_mem_SimpleDP_8Kx512b(input clka, input ena, input [0:0] wea, input [12:0] addra, input [515:0] dina,
                                input clkb, input enb, input [12:0] addrb, output reg [515:0] doutb);
  reg [515:0] m [0:8191];
  always @(posedge clka) if (ena && wea[0]) m[addra] <= dina;
  always @(posedge clkb) if (enb) doutb <= m[addrb];
endmodule

module input_buffer_status_simpleDP_8Kx8b(input clka, input ena, input [0:0] wea, input [12:0] addra, input [47:0] dina,
                                input clkb, input enb, input [12:0] addrb, output reg [47:0] doutb);
  reg [47:0] m [0:8191];
  always @(posedge clka) if (ena && wea[0]) m[addra] <= dina;
  always @(posedge clkb) if (enb) doutb <= m[addrb];
endmodule

// TDP with primitive output registers => latency 2
module blk_mem_DP_histo_1Kx32b(input clka, input ena, input [0:0] wea, input [9:0] addra, input [31:0] dina, output reg [31:0] douta,
                               input clkb, input enb, input [0:0] web, input [9:0] addrb, input [31:0] dinb, output reg [31:0] doutb);
  reg [31:0] m [0:1023]; reg [31:0] qa, qb; integer i; initial for (i=0;i<1024;i=i+1) m[i]=0;
  always @(posedge clka) begin if (wea[0]) m[addra] <= dina; else qa <= m[addra]; douta <= qa; end
  always @(posedge clkb) begin qb <= m[addrb]; doutb <= qb; end
endmodule

// AXI-Stream PACKET FIFO: m_axis_tvalid only once a whole packet is in (zuc_module relies on it)
module axis_512x512b_fifo(input s_aclk, input s_aresetn,
  input s_axis_tvalid, output s_axis_tready, input [511:0] s_axis_tdata, input s_axis_tlast, input [0:0] s_axis_tuser,
  output m_axis_tvalid, input m_axis_tready, output [511:0] m_axis_tdata, output m_axis_tlast, output [0:0] m_axis_tuser,
  output [9:0] axis_data_count);
  wire [10:0] cnt;
  xpm_fifo_axis #(
    .CLOCKING_MODE("common_clock"), .FIFO_DEPTH(1024), .FIFO_MEMORY_TYPE("block"), .PACKET_FIFO("true"),
    .TDATA_WIDTH(512), .TUSER_WIDTH(1), .TID_WIDTH(1), .TDEST_WIDTH(1),
    .USE_ADV_FEATURES("0004"), .WR_DATA_COUNT_WIDTH(11), .ECC_MODE("no_ecc"), .SIM_ASSERT_CHK(0)
  ) u (
    .s_aclk(s_aclk), .s_aresetn(s_aresetn), .m_aclk(s_aclk),
    .s_axis_tvalid(s_axis_tvalid), .s_axis_tready(s_axis_tready), .s_axis_tdata(s_axis_tdata),
    .s_axis_tkeep({64{1'b1}}), .s_axis_tstrb({64{1'b1}}), .s_axis_tlast(s_axis_tlast), .s_axis_tuser(s_axis_tuser),
    .s_axis_tid(1'b0), .s_axis_tdest(1'b0),
    .m_axis_tvalid(m_axis_tvalid), .m_axis_tready(m_axis_tready), .m_axis_tdata(m_axis_tdata),
    .m_axis_tkeep(), .m_axis_tstrb(), .m_axis_tlast(m_axis_tlast), .m_axis_tuser(m_axis_tuser), .m_axis_tid(), .m_axis_tdest(),
    .wr_data_count_axis(cnt), .rd_data_count_axis(), .prog_full_axis(), .prog_empty_axis(),
    .almost_full_axis(), .almost_empty_axis(), .injectsbiterr_axis(1'b0), .injectdbiterr_axis(1'b0),
    .sbiterr_axis(), .dbiterr_axis()
  );
  // the AFU never lets it exceed 514, so the low 10 bits are exact
  assign axis_data_count = cnt[9:0];
endmodule

// small FIFOs: distributed RAM, as in the original IP
module axis_32x8b_fifo(input s_aclk, input s_aresetn, input s_axis_tvalid, output s_axis_tready, input [7:0] s_axis_tdata,
  output m_axis_tvalid, input m_axis_tready, output [7:0] m_axis_tdata);
  reg [7:0] q [0:255]; reg [8:0] wp, rp, cnt;
  wire wr = s_axis_tvalid && s_axis_tready; wire rd = m_axis_tvalid && m_axis_tready;
  assign s_axis_tready = s_aresetn && (cnt < 256); assign m_axis_tvalid = (cnt != 0); assign m_axis_tdata = q[rp[7:0]];
  always @(posedge s_aclk) if (!s_aresetn) begin wp<=0; rp<=0; cnt<=0; end else begin
    if (wr) begin q[wp[7:0]] <= s_axis_tdata; wp <= wp+1; end
    if (rd) rp <= rp+1; cnt <= cnt + wr - rd; end
endmodule

module fifo_32x20b(input clk, input srst, input [19:0] din, input wr_en, input rd_en, output [19:0] dout,
  output full, output empty, output valid, output [5:0] data_count, output wr_rst_busy, output rd_rst_busy);
  reg [19:0] q [0:31]; reg [5:0] wp, rp, cnt;
  assign full = (cnt == 32); assign empty = (cnt == 0); assign valid = !empty; assign dout = q[rp[4:0]];
  assign data_count = cnt; assign wr_rst_busy = 1'b0; assign rd_rst_busy = 1'b0;
  always @(posedge clk) if (srst) begin wp<=0; rp<=0; cnt<=0; end else begin
    if (wr_en && !full) begin q[wp[4:0]] <= din; wp <= wp+1; end
    if (rd_en && !empty) rp <= rp+1;
    cnt <= cnt + (wr_en && !full) - (rd_en && !empty); end
endmodule
