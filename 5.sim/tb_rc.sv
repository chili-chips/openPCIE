//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   Test bench of the Artix-7 RC model (make RC=model): the model, pcieVHost
//   on node 2 (usercode/VUserMain2.cpp), in place of the RC_opensource DUT,
//   against the same pcieVHost endpoint as tb.sv, on node 1
//   (usercode/VUserMain1.cpp). They meet on a 16-bit PIPE, x1 or x4, Gen1 or
//   Gen2, through one register stage each way, as in
//   models/phy_top.EP-PIPE-BFM.sv.
//
//        u_rc  PIPExN__pcieVHost, EndPoint = 0   <-- the RC model
//          |   PIPE
//        u_ep  PIPExN__pcieVHost, EndPoint = 1   <-- the endpoint
//
//   The module is called tb, like tb.sv: the pcieVHost headers call tb.Fatal.
//   The run ends when the model has checked the endpoint's answers (RC MODEL
//   RESULT), or at the time limit of run_us.cfg.
//==========================================================================

`timescale 1ps/1ps

`include "pciexpress_header.v"

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
// Clocks and reset: PCLK 125 MHz at Gen1, 250 MHz at Gen2; each model makes
// its symbol clock from it (models/PIPExN__pcieVHost.sv). The RC model sets
// the rate: its GEN2_CLK register, which it writes in Recovery.Speed with
// both sides idle.
// Resets are released with nonblocking assignments: a release on a symbol
// clock edge then comes after that edge on every simulator.
//--------------------------------------------------------------

  localparam int PCLK_HALF_GEN1_PS   = 4000;  // 125 MHz
  localparam int PCLK_HALF_GEN2_PS   = 2000;  // 250 MHz

  bit   pclk;
  bit   nreset;             // PERST#: the RC model
  bit   ep_nreset;          // the endpoint
  bit   rate_sel;
  logic rc_gen2;

  initial begin
    pclk      = 1'b0;
    nreset    = 1'b0;
    ep_nreset = 1'b0;
    rate_sel  = 1'b0;
  end

  // A hard 0/1: an X delay expression is a zero delay, which hangs the simulator
  always @(rc_gen2) rate_sel = (rc_gen2 === 1'b1);

  always #((rate_sel ? PCLK_HALF_GEN2_PS : PCLK_HALF_GEN1_PS) * 1ps) pclk = ~pclk;

  initial begin
    get_run_length();
    // tb.sv releases PERST# at 1 us, and the endpoint 487 ns later, once the
    // RC's PIPE clock runs
    #(1us)   nreset    <= 1'b1;
    #(487ns) ep_nreset <= 1'b1;
    #(RUN_SIM_US * 1us - 1us - 487ns);
    $fatal(1, "time limit of %0d us: no RC MODEL RESULT", RUN_SIM_US);
  end

//--------------------------------------------------------------
// The link: one register stage each way
//--------------------------------------------------------------

`ifndef PCIE_LANES
  `define PCIE_LANES 1
`endif
  localparam int L = `PCIE_LANES;   // make SIM_LANES: 1 or 4

  bit   [16*L-1:0] rc_rx_data,  ep_rx_data;
  bit   [ 2*L-1:0] rc_rx_datak, ep_rx_datak;
  logic [16*L-1:0] rc_tx_data,  ep_tx_data;
  logic [ 2*L-1:0] rc_tx_datak, ep_tx_datak;

  // the link is idle in reset (NOSIG with the K flag). make SIM_LANE_REVERSE=1:
  // lane l of the RC goes to lane L-1-l of the endpoint, both ways (4.2.4.9.1)
  for (genvar l = 0; l < L; l++) begin : g_lane
`ifdef SIM_LANE_REVERSE
    localparam int M = L-1-l;
`else
    localparam int M = l;
`endif
    always_ff @(posedge pclk or negedge nreset) begin
      if (!nreset) begin
        {rc_rx_data[16*l +: 16], rc_rx_datak[2*l +: 2]} <= {{2{`NOSIG}}, 2'b11};
        {ep_rx_data[16*M +: 16], ep_rx_datak[2*M +: 2]} <= {{2{`NOSIG}}, 2'b11};
      end
      else begin
        {rc_rx_data[16*l +: 16], rc_rx_datak[2*l +: 2]} <= {ep_tx_data[16*M +: 16], ep_tx_datak[2*M +: 2]};
        {ep_rx_data[16*M +: 16], ep_rx_datak[2*M +: 2]} <= {rc_tx_data[16*l +: 16], rc_tx_datak[2*l +: 2]};
      end
    end
  end

//--------------------------------------------------------------
// The RC model and the endpoint
//--------------------------------------------------------------

  PIPExN__pcieVHost #(
    .NodeNum   (2),      // VUserMain2.cpp -- the Artix-7 RC model
    .EndPoint  (0),
    .Lanes     (L),
    .DataWidth (16)
  ) u_rc (
    .pclk      (pclk),
    .gen2      (rate_sel),
    .nreset    (nreset),
    .Gen2ClkSel(rc_gen2),
    .RxData    (rc_rx_data),
    .RxDataK   (rc_rx_datak),
    .TxData    (rc_tx_data),
    .TxDataK   (rc_tx_datak),
    .dll_clk   (pclk),
    .pipe_dn_pclk (), .pipe_dn_data (), .pipe_dn_datak (),
    .pipe_up_pclk (), .pipe_up_data (), .pipe_up_datak (),
    .dll_rx_data  (), .dll_rx_tlp_valid (), .dll_rx_tlp_first (), .dll_rx_tlp_last (),
    .dll_rx_hdr_credit  (), .dll_rx_data_credit (),
    .dll_rx_update_type (), .dll_rx_packet_type (), .dll_rx_dllp_valid (),
    .dllp_clk     (), .dllp_dn_data (), .dllp_dn_valid (), .dllp_up_data (), .dllp_up_valid (),
    .tx_clk       (), .tx_tlp_data  (), .tx_tlp_sop  (), .tx_tlp_eop  (), .tx_tlp_valid  (),
    .tx_peer_data (), .tx_peer_sop  (), .tx_peer_eop (), .tx_peer_valid (),
    .rx_clk       (), .rx_tlp_data  (), .rx_tlp_sop  (), .rx_tlp_eop  (), .rx_tlp_valid  (),
    .rx_peer_data (), .rx_peer_sop  (), .rx_peer_eop (), .rx_peer_valid ()
  );

  PIPExN__pcieVHost #(
    .NodeNum   (1),      // VUserMain1.cpp -- the endpoint, as in tb.sv
    .EndPoint  (1),
    .Lanes     (L),
    .DataWidth (16)
  ) u_ep (
    .pclk      (pclk),
    .gen2      (rate_sel),
    .nreset    (ep_nreset),
    .Gen2ClkSel(),
    .RxData    (ep_rx_data),
    .RxDataK   (ep_rx_datak),
    .TxData    (ep_tx_data),
    .TxDataK   (ep_tx_datak),
    .dll_clk   (pclk),
    .pipe_dn_pclk (), .pipe_dn_data (), .pipe_dn_datak (),
    .pipe_up_pclk (), .pipe_up_data (), .pipe_up_datak (),
    .dll_rx_data  (), .dll_rx_tlp_valid (), .dll_rx_tlp_first (), .dll_rx_tlp_last (),
    .dll_rx_hdr_credit  (), .dll_rx_data_credit (),
    .dll_rx_update_type (), .dll_rx_packet_type (), .dll_rx_dllp_valid (),
    .dllp_clk     (), .dllp_dn_data (), .dllp_dn_valid (), .dllp_up_data (), .dllp_up_valid (),
    .tx_clk       (), .tx_tlp_data  (), .tx_tlp_sop  (), .tx_tlp_eop  (), .tx_tlp_valid  (),
    .tx_peer_data (), .tx_peer_sop  (), .tx_peer_eop (), .tx_peer_valid (),
    .rx_clk       (), .rx_tlp_data  (), .rx_tlp_sop  (), .rx_tlp_eop  (), .rx_tlp_valid  (),
    .rx_peer_data (), .rx_peer_sop  (), .rx_peer_eop (), .rx_peer_valid ()
  );

//--------------------------------------------------------------
// Waveforms: both wrappers. tb.u_ep has the signals tb.sv dumps for its
// endpoint, so the same scripts and decoders read both runs
//--------------------------------------------------------------

`ifdef DUMP_VCD
  initial begin
    $dumpfile("tb.vcd");
    $dumpvars(1, tb.u_ep);
    $dumpvars(1, tb.u_rc);
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
