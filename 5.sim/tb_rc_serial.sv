//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   Test bench of the Artix-7 RC model on the serial line (make RC=model
//   IFACE=serial): the model, pcieVHost on node 2 (usercode/VUserMain2.cpp),
//   against the same pcieVHost endpoint as tb.sv, on node 1
//   (usercode/VUserMain1.cpp). Both are upstream's PcieVhostSerial: the model
//   does 8b/10b and scrambling, as on hardware, and the wrapper puts the
//   10-bit symbols on a 1-bit line per lane, x1 or x4 (make SIM_LANES).
//   Electrical idle is a constant 0 on the line.
//
//        u_rc  PcieVhostSerial, EndPoint = 0   <-- the RC model
//          |   ser_dn, ser_up: a line per lane, 2.5 or 5 Gb/s
//        u_ep  PcieVhostSerial, EndPoint = 1   <-- the endpoint
//
//   Gen2Clk = 1: the wrappers take the Gen2 clocks and halve them at Gen1;
//   the model's GEN2_CLK register selects.
//
//   The module is called tb, like tb.sv: the pcieVHost headers call tb.Fatal.
//   The run ends when the model has checked the endpoint's answers (RC MODEL
//   RESULT), or at the time limit of run_us.cfg.
//==========================================================================

`timescale 1ps/1ps

module tb;

  int RUN_SIM_US;

  task automatic get_run_length();
    int fh;
    int code;

    RUN_SIM_US = 2000;
    fh = $fopen("run_us.cfg", "r");
    if (fh != 0) begin
      code = $fscanf(fh, "%d", RUN_SIM_US);
      $fclose(fh);
    end
  endtask

//--------------------------------------------------------------
// Clocks and reset: the Gen2 symbol clock (500 MHz) and bit clock (5 GHz);
// the wrappers halve both at Gen1. Resets as in tb.sv: PERST# at 1 us, the
// endpoint 487 ns later
//--------------------------------------------------------------

  localparam int CLK_HALF_PS = 1000;  // 500 MHz
  localparam int SER_HALF_PS = 100;   // 5 GHz

  bit   clk;
  bit   serclk;
  bit   nreset;             // PERST#: the RC model
  bit   ep_nreset;          // the endpoint

  initial begin
    clk       = 1'b0;
    serclk    = 1'b0;
    nreset    = 1'b0;
    ep_nreset = 1'b0;
  end

  always #(CLK_HALF_PS * 1ps) clk    = ~clk;
  always #(SER_HALF_PS * 1ps) serclk = ~serclk;

  initial begin
    get_run_length();
    #(1us)   nreset    <= 1'b1;   // nonblocking, as in tb_rc.sv
    #(487ns) ep_nreset <= 1'b1;
    #(RUN_SIM_US * 1us - 1us - 487ns);
    $fatal(1, "time limit of %0d us: no RC MODEL RESULT", RUN_SIM_US);
  end

//--------------------------------------------------------------
// The lines, one bit per lane each way
//--------------------------------------------------------------

`ifndef PCIE_LANES
  `define PCIE_LANES 1
`endif
  localparam int L = `PCIE_LANES;   // make SIM_LANES: 1 or 4

  logic [15:0] rc_out, ep_out;   // what each wrapper drives, all 16 lanes
  logic [15:0] rc_in,  ep_in;    // what each receives

  // make SIM_POLARITY=1: every line with D+ and D- swapped, so each receiver
  // must invert its lane in Polling (4.2.4.4); electrical idle, a constant
  // line, stays idle inverted: the receiver takes all 0s or all 1s as idle.
  // make SIM_LANE_REVERSE=1: lane l of the RC goes to lane L-1-l of the
  // endpoint, both ways (4.2.4.9.1). Lanes past L carry nothing
  for (genvar l = 0; l < 16; l++) begin : g_lane
    if (l < L) begin : g_used
`ifdef SIM_LANE_REVERSE
      localparam int M = L-1-l;
`else
      localparam int M = l;
`endif
`ifdef SIM_POLARITY
      assign ep_in[M] = ~rc_out[l];
      assign rc_in[l] = ~ep_out[M];
`else
      assign ep_in[M] = rc_out[l];
      assign rc_in[l] = ep_out[M];
`endif
    end else begin : g_unused
      assign ep_in[l] = 1'b0;
      assign rc_in[l] = 1'b0;
    end
  end

  logic ser_dn, ser_up;   // lane 0 each way, for the waveforms
  assign ser_dn = rc_out[0];
  assign ser_up = ep_out[0];

//--------------------------------------------------------------
// The RC model and the endpoint
//--------------------------------------------------------------

  PcieVhostSerial #(
    .LinkWidth (L),
    .NodeNum   (2),      // VUserMain2.cpp -- the Artix-7 RC model
    .EndPoint  (0),
    .Gen2Clk   (1)
  ) u_rc (
    .Clk       (clk),
    .SerClk    (serclk),
    .notReset  (nreset),
    .ClkOut    (),
    .SerClkOut (),
    .LinkIn0   (rc_in[0]),  .LinkIn1  (rc_in[1]),  .LinkIn2  (rc_in[2]),  .LinkIn3  (rc_in[3]),
    .LinkIn4   (rc_in[4]),  .LinkIn5  (rc_in[5]),  .LinkIn6  (rc_in[6]),  .LinkIn7  (rc_in[7]),
    .LinkIn8   (rc_in[8]),  .LinkIn9  (rc_in[9]),  .LinkIn10 (rc_in[10]), .LinkIn11 (rc_in[11]),
    .LinkIn12  (rc_in[12]), .LinkIn13 (rc_in[13]), .LinkIn14 (rc_in[14]), .LinkIn15 (rc_in[15]),
    .LinkOut0  (rc_out[0]),  .LinkOut1  (rc_out[1]),  .LinkOut2  (rc_out[2]),  .LinkOut3  (rc_out[3]),
    .LinkOut4  (rc_out[4]),  .LinkOut5  (rc_out[5]),  .LinkOut6  (rc_out[6]),  .LinkOut7  (rc_out[7]),
    .LinkOut8  (rc_out[8]),  .LinkOut9  (rc_out[9]),  .LinkOut10 (rc_out[10]), .LinkOut11 (rc_out[11]),
    .LinkOut12 (rc_out[12]), .LinkOut13 (rc_out[13]), .LinkOut14 (rc_out[14]), .LinkOut15 (rc_out[15])
  );

  PcieVhostSerial #(
    .LinkWidth (L),
    .NodeNum   (1),      // VUserMain1.cpp -- the endpoint, as in tb.sv
    .EndPoint  (1),
    .Gen2Clk   (1)
  ) u_ep (
    .Clk       (clk),
    .SerClk    (serclk),
    .notReset  (ep_nreset),
    .ClkOut    (),
    .SerClkOut (),
    .LinkIn0   (ep_in[0]),  .LinkIn1  (ep_in[1]),  .LinkIn2  (ep_in[2]),  .LinkIn3  (ep_in[3]),
    .LinkIn4   (ep_in[4]),  .LinkIn5  (ep_in[5]),  .LinkIn6  (ep_in[6]),  .LinkIn7  (ep_in[7]),
    .LinkIn8   (ep_in[8]),  .LinkIn9  (ep_in[9]),  .LinkIn10 (ep_in[10]), .LinkIn11 (ep_in[11]),
    .LinkIn12  (ep_in[12]), .LinkIn13 (ep_in[13]), .LinkIn14 (ep_in[14]), .LinkIn15 (ep_in[15]),
    .LinkOut0  (ep_out[0]),  .LinkOut1  (ep_out[1]),  .LinkOut2  (ep_out[2]),  .LinkOut3  (ep_out[3]),
    .LinkOut4  (ep_out[4]),  .LinkOut5  (ep_out[5]),  .LinkOut6  (ep_out[6]),  .LinkOut7  (ep_out[7]),
    .LinkOut8  (ep_out[8]),  .LinkOut9  (ep_out[9]),  .LinkOut10 (ep_out[10]), .LinkOut11 (ep_out[11]),
    .LinkOut12 (ep_out[12]), .LinkOut13 (ep_out[13]), .LinkOut14 (ep_out[14]), .LinkOut15 (ep_out[15])
  );

//--------------------------------------------------------------
// Waveforms: each side's 10-bit symbols (pvh.LinkOut0, LinkIn0, on ClkOut)
// and its transaction taps (pvh: Tlp*, Dllp*). With DUMP_LINE also the lines:
// a few G events a second
//--------------------------------------------------------------

`ifdef DUMP_VCD
  initial begin
    $dumpfile("tb.vcd");
    $dumpvars(1, tb.u_rc.pvh, tb.u_ep.pvh);
`ifdef DUMP_LINE
    $dumpvars(1, tb.ser_dn, tb.ser_up, tb.rc_out, tb.ep_out);
`endif
  end
`endif

// Top level fatal task, called by the `fatal macro of the pcieVHost headers
task Fatal;
begin
    $fatal(1, "pcieVHost fatal: the RC model's FAIL, or a bad VProc access");
end
endtask

endmodule: tb

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// -----------------------------------------------------------------------------
