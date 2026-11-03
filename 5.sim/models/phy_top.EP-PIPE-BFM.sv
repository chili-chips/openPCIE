//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   Co-simulation stand-in for 2.rtl/0.common.opensource/src/pcie/phy_top.sv
//
//   Same module name, same port list, so it is a drop-in replacement: the
//   simulation file list picks this file INSTEAD of the synthesis one and
//   nothing else in the design changes. It is the exact same trick the test
//   bench uses for the CPU with soc_cpu.VPROC.sv.
//
//   What it replaces
//   ----------------
//   The synthesis phy_top wraps gtp_wrapper -> lane_xcvr -> GTPE2_CHANNEL,
//   i.e. the 7-series gigabit transceiver, and presents a PIPE interface to
//   the PCIE_2_1 hard macro above it. The transceiver is what turns PIPE
//   symbols into a 5 GT/s serial bit stream.
//
//   The pcievhost VIP models the link at PIPE symbol level, not at serial bit
//   level, so the transceiver has to come out for co-simulation. This module
//   therefore provides:
//
//     1. a behavioural PIPE PHY -- the sideband handshake the PCIE_2_1 LTSSM
//        needs from a PHY: PHYSTATUS, receiver detection, RXVALID, RXELECIDLE
//        (models/pipe_phy_core.sv), and the PCLK/rate handling
//     2. the link partner itself -- a pcieVHost in our wrapper
//        PIPExN__pcieVHost, configured as an ENDPOINT, so the openPCIE Root
//        Complex has something to enumerate. x1 or x4, as PCIE_LANES
//
//        PCIE_2_1  <--16-bit PIPE-->  this PHY  <--16-bit PIPE-->  pcieVHost EP
//                     per lane                      per lane
//              (pcie_core.sv)                                 (VUserMain1)
//
//   Clocking
//   --------
//   In hardware the GT passes the 100 MHz refclk out as TXOUTCLK, and
//   clk_rst_gen's MMCM makes PCLK (125 MHz Gen1 / 250 MHz Gen2) and
//   USERCLK1/2 (62.5 MHz at x1, 125 MHz at x4 Gen1, 250 MHz at x4 Gen2). Here
//   TXOUTCLK is a behavioural oscillator and everything downstream is the real
//   clk_rst_gen, so the clock topology the design sees is unchanged.
//
//   The pcievhost wrapper needs a symbol clock at twice the 16-bit PIPE rate;
//   it is generated from the same behavioural source so the two stay edge
//   aligned, and it follows the link rate when the LTSSM changes speed.
//==========================================================================

`timescale 1ps/1ps

module phy_top
  import link_pkg::*;
(
  input  logic [5:0]                             pl_ltssm_state,

  pipe_if.phy                              pipe_mac,

  output logic [PCIE_LANES-1:0]     pci_exp_txn,
  output logic [PCIE_LANES-1:0]     pci_exp_txp,
  input  logic [PCIE_LANES-1:0]     pci_exp_rxn,
  input  logic [PCIE_LANES-1:0]     pci_exp_rxp,

  input  logic                                   refclk_p,
  input  logic                                   refclk_n,
  input  logic                                   rst125_n,  // released on clk125
  input  logic                                   pipe_rst_n,    // released on pipe_clk

  input  logic                                   pipe_clk_in,
  input  logic                                   pipe_drp_clk_in,
  input  logic                                   pipe_mmcm_lock_in,
  output logic                                   pipe_txoutclk_out,
  output logic                      pipe_clk_sel_out,

  output logic                                  phy_rdy_n
);

  // The serial pads are unused -- the link is modelled at PIPE level
  assign pci_exp_txp = '0;
  assign pci_exp_txn = '0;

  //------------------------------------------------------------------
  // Behavioural clocks
  //------------------------------------------------------------------
  // TXOUTCLK is what the GT recovers and feeds to clk_rst_gen's MMCM. It has to
  // be 100 MHz: clk_rst_gen is parameterised with PCIE_REFCLK_FREQ = 0, i.e.
  // CLKIN1_PERIOD = 10 ns and CLKFBOUT_MULT_F = 10, so the VCO lands on
  // 1000 MHz and the outputs come out as
  //     CLKOUT0 = 1000/8  = 125   MHz   PCLK, Gen1
  //     CLKOUT1 = 1000/4  = 250   MHz   PCLK, Gen2
  //     CLKOUT2 = 1000/(32 >> USER_CLK_FREQ) = 62.5 / 125 / 250 MHz   USERCLK1/2
  // Feed it anything else and every clock in the design scales with it -- at
  // 125 MHz in, PCLK becomes 156.25 MHz and the PIPE then over-samples the
  // link partner, duplicating one symbol in five.
  localparam int TXOUTCLK_HALF_PS = 5000;   // 100 MHz

  bit   txoutclk;

  always #(TXOUTCLK_HALF_PS * 1ps) txoutclk = ~txoutclk;

  // Rate follows PIPETXRATE, which the LTSSM drives on a speed change.
  // It has to be resolved to a hard 0/1 first: PIPETXRATE is X before the hard
  // macro comes out of reset, and an X delay expression is a zero delay, which
  // hangs the simulator at time 0.
  bit   rate_sel;

  initial begin
    txoutclk = 1'b0;
    rate_sel = 1'b0;
  end

  always @(pipe_mac.tx_ctrl.rate) rate_sel = (pipe_mac.tx_ctrl.rate === 1'b1);

  assign pipe_txoutclk_out = txoutclk;

  // PCLK, USERCLK1 and USERCLK2 come straight back from clk_rst_gen, exactly as
  // gtp_wrapper passes them through in the synthesis build
  logic  pipe_clk;
  assign pipe_clk = pipe_clk_in;

  // Tell clk_rst_gen which PCLK to select. Gen3 does not exist on this device.
  assign pipe_clk_sel_out = pipe_mac.tx_ctrl.rate;

  //------------------------------------------------------------------
  // pcievhost endpoint, connected over a 16-bit PIPE, one lane per lane of
  // the hard macro (PCIE_LANES: 1 or 4, make SIM_LANES)
  //------------------------------------------------------------------
  // What we transmit is what the model receives, and vice versa. Lane l is
  // bits [16*l +: 16] (data) and [2*l +: 2] (K flags).
  logic [16*PCIE_LANES-1:0] ep_rx_data;      // -> model
  logic [ 2*PCIE_LANES-1:0] ep_rx_datak;
  logic [16*PCIE_LANES-1:0] ep_tx_data;      // <- model
  logic [ 2*PCIE_LANES-1:0] ep_tx_datak;

  //------------------------------------------------------------------
  // The PHY: PIPE handshakes and electrical idle, for the hard macro's
  // PIPE (models/pipe_phy_core.sv, shared with openCologne-PCIE). The
  // hard macro's lanes are packed for it: lane l is [16*l +: 16], [2*l +: 2]
  //------------------------------------------------------------------
  logic [16*PCIE_LANES-1:0] mac_tx_data,  mac_rx_data;
  logic [ 2*PCIE_LANES-1:0] mac_tx_datak, mac_rx_datak;
  logic [   PCIE_LANES-1:0] mac_tx_elec_idle, mac_rx_valid, mac_rx_elec_idle;
  logic                     phy_status;
  logic [              2:0] rx_status;
  logic                     phy_rdy;

  pipe_phy_core #(
    .LANES         (PCIE_LANES),
    .BYTES         (2),     // the 7-series 2-symbol PIPE
    .TX_FLOP       (1),     // the model sees a registered bus
    .RDY_LAT       (256),   // in hardware: the GT reset FSM and the MMCM lock
    .RST_LAT       (0),     // PhyStatus low in PIPE reset: high there, the hard
                            // macro falls back from Polling.Active to
                            // Detect.Quiet 3 times after the reset
    .PHYSTATUS_LAT (8),
    .RCVRDET_LAT   (16)
  ) u_phy (
    .pclk          (pipe_clk),
    .rst_n         (pipe_rst_n),
    .phy_rdy       (phy_rdy),

    .mac_reset     (pipe_mac.tx_ctrl.reset),
    .powerdown     (pipe_mac.tx[0].powerdown),
    .rate          (pipe_mac.tx_ctrl.rate),
    .tx_detect_rx  (pipe_mac.tx_ctrl.rcvr_det),
    .tx_elec_idle  (mac_tx_elec_idle),
    .tx_data       (mac_tx_data),
    .tx_datak      (mac_tx_datak),

    .phy_status    (phy_status),
    .rx_status     (rx_status),
    .rx_valid      (mac_rx_valid),
    .rx_elec_idle  (mac_rx_elec_idle),
    .rx_data       (mac_rx_data),
    .rx_datak      (mac_rx_datak),

    .rcvr_present  (1'b1),
    .host_rxdata   (ep_rx_data),
    .host_rxdatak  (ep_rx_datak),
    .host_txdata   (ep_tx_data),
    .host_txdatak  (ep_tx_datak)
  );

  assign phy_rdy_n = ~phy_rdy;

  // The squelch, per lane, under its old name for the dumps (tb.sv, DUMP_VCD)
  logic [PCIE_LANES-1:0] rx_active;
  assign rx_active = mac_rx_valid;

  for (genvar l = 0; l < PCIE_LANES; l++) begin : g_lane
    assign mac_tx_data     [16*l +: 16]   = pipe_mac.tx[l].data;
    assign mac_tx_datak    [ 2*l +:  2]   = pipe_mac.tx[l].char_is_k;
    assign mac_tx_elec_idle[l]            = pipe_mac.tx[l].elec_idle;

    assign pipe_mac.rx[l].data          = mac_rx_data [16*l +: 16];
    assign pipe_mac.rx[l].char_is_k     = mac_rx_datak[ 2*l +:  2];
    assign pipe_mac.rx[l].valid         = mac_rx_valid[l];
    assign pipe_mac.rx[l].chanisaligned = mac_rx_valid[l];
    assign pipe_mac.rx[l].status        = rx_status;
    assign pipe_mac.rx[l].phy_status    = phy_status;
    assign pipe_mac.rx[l].elec_idle     = mac_rx_elec_idle[l];
  end

  // make SIM_LANE_REVERSE=1: lane l of the hard macro goes to lane
  // PCIE_LANES-1-l of the endpoint, both ways (4.2.4.9.1). The dumps keep the
  // hard macro's lane order
  logic [16*PCIE_LANES-1:0] ep_rx_data_m, ep_tx_data_m;   // the model's lanes
  logic [ 2*PCIE_LANES-1:0] ep_rx_datak_m, ep_tx_datak_m;

  for (genvar l = 0; l < PCIE_LANES; l++) begin : g_rev
`ifdef SIM_LANE_REVERSE
    localparam int M = PCIE_LANES-1-l;
`else
    localparam int M = l;
`endif
    assign ep_rx_data_m [16*M +: 16] = ep_rx_data [16*l +: 16];
    assign ep_rx_datak_m[ 2*M +:  2] = ep_rx_datak[ 2*l +:  2];
    assign ep_tx_data   [16*l +: 16] = ep_tx_data_m [16*M +: 16];
    assign ep_tx_datak  [ 2*l +:  2] = ep_tx_datak_m[ 2*M +:  2];
  end

  // The model in our wrapper: the same link, plus its TLPs and DLLPs at the
  // Data Link and Transaction layers, for the dump (models/PIPExN__pcieVHost.sv)
  PIPExN__pcieVHost #(
    .NodeNum   (1),      // VUserMain1.cpp -- the endpoint program
    .EndPoint  (1),      // enables the config space and auto-completions
    .Lanes     (PCIE_LANES),
    .DataWidth (16)      // matches the 7-series 2-symbol PIPE
  ) u_pcie_ep (
    .pclk      (pipe_clk),
    .gen2      (rate_sel),   // its symbol clock follows the rate
    .nreset    (pipe_rst_n),
    .Gen2ClkSel(),       // the hard macro sets the rate (PIPETXRATE)

    .RxData    (ep_rx_data_m),
    .RxDataK   (ep_rx_datak_m),

    .TxData    (ep_tx_data_m),
    .TxDataK   (ep_tx_datak_m),

    .dll_clk   (pipe_clk),

    // the decoder page: read from the dump only
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

  // All lanes of an x4 link, for the multi-lane decoders; dumped at x4 only
  // (tb.sv). At x1, lanes 1 to 3 are 0.
  pipe_x4_view #(
    .DataWidth (16)
  ) u_x4 (
    .pclk (pipe_clk),
    .dn   (64'(ep_rx_data)),
    .dnk  ( 8'(ep_rx_datak)),
    .up   (64'(ep_tx_data)),
    .upk  ( 8'(ep_tx_datak))
  );

  //------------------------------------------------------------------
  // Ordered-set monitor
  //------------------------------------------------------------------
  // Decodes the symbol stream in both directions and reports each change of
  // ordered set. This is the quickest way to see which side of the link is
  // not answering during training. Enable with +define+PIPE_OS_TRACE.
`ifdef PIPE_OS_TRACE
  // TS1 and TS2 are 16 symbols: COM then 14 identifier-bearing symbols, of
  // which symbols 6..15 are D10.2 (0x4A) for TS1 and D5.2 (0x45) for TS2.
  localparam logic [7:0] SYM_COM = 8'hBC;
  localparam logic [7:0] SYM_TS1 = 8'h4A;
  localparam logic [7:0] SYM_TS2 = 8'h45;
  localparam logic [7:0] SYM_IDL = 8'h7C;   // K28.3, part of EIOS
  localparam logic [7:0] SYM_SKP = 8'h1C;   // K28.0

  // One monitor instance per direction, fed a symbol at a time
  function automatic string os_name(input logic [7:0] id);
    case (id)
      SYM_TS1: return "TS1";
      SYM_TS2: return "TS2";
      SYM_IDL: return "EIOS/IDL";
      SYM_SKP: return "SKP";
      default: return "other";
    endcase
  endfunction

  // TX side (RC -> EP)
  int       tx_pos;
  bit [7:0] tx_last_id;
  // RX side (EP -> RC)
  int       rx_pos;
  bit [7:0] rx_last_id;

  initial begin
    tx_pos = -1; tx_last_id = 8'hFF;
    rx_pos = -1; rx_last_id = 8'hFF;
  end

  bit [7:0] tx_link, tx_lane, tx_last_link, tx_last_lane;

  task automatic feed_tx(input logic [7:0] sym, input logic is_k);
    if (is_k && sym == SYM_COM) tx_pos = 0;
    else if (tx_pos >= 0) begin
      tx_pos = tx_pos + 1;
      if (tx_pos == 1) tx_link = sym;
      if (tx_pos == 2) tx_lane = sym;
      if (tx_pos == 6) begin
        if (sym !== tx_last_id || tx_link !== tx_last_link || tx_lane !== tx_last_lane) begin
          tx_last_id   = sym;
          tx_last_link = tx_link;
          tx_last_lane = tx_lane;
          $display("%t  PIPE   RC --> EP  %s link=%02h lane=%02h", $time,
                   os_name(sym), tx_link, tx_lane);
        end
        tx_pos = -1;
      end
    end
  endtask

  bit [7:0] rx_link, rx_lane, rx_last_link, rx_last_lane;

  task automatic feed_rx(input logic [7:0] sym, input logic is_k);
    if (is_k && sym == SYM_COM) rx_pos = 0;
    else if (rx_pos >= 0) begin
      rx_pos = rx_pos + 1;
      if (rx_pos == 1) rx_link = sym;
      if (rx_pos == 2) rx_lane = sym;
      if (rx_pos == 6) begin
        if (sym !== rx_last_id || rx_link !== rx_last_link || rx_lane !== rx_last_lane) begin
          rx_last_id   = sym;
          rx_last_link = rx_link;
          rx_last_lane = rx_lane;
          $display("%t  PIPE   EP --> RC  %s link=%02h lane=%02h", $time,
                   os_name(sym), rx_link, rx_lane);
        end
        rx_pos = -1;
      end
    end
  endtask

  always @(posedge pipe_clk) if (pipe_rst_n) begin
    feed_tx(pipe_mac.tx[0].data[7:0],   pipe_mac.tx[0].char_is_k[0]);
    feed_tx(pipe_mac.tx[0].data[15:8],  pipe_mac.tx[0].char_is_k[1]);
    feed_rx(ep_tx_data[7:0],        ep_tx_datak[0]);
    feed_rx(ep_tx_data[15:8],       ep_tx_datak[1]);
  end
`endif

  // Raw symbol dump (+define+PIPE_RAW_DUMP): lane 0's first 40 words in each
  // direction once the partner drives, from make PLUSARGS=PIPE_DUMP_FROM=<ps>
  // (default 0). Ground truth for alignment problems.
`ifdef PIPE_RAW_DUMP
  int     dump_n;     // 0 at time 0
  longint dump_from;

  initial if (!$value$plusargs("PIPE_DUMP_FROM=%d", dump_from)) dump_from = 0;

  always @(posedge pipe_clk)
    if (pipe_rst_n && mac_rx_valid[0] && dump_n < 40 && $time > dump_from) begin
      $display("%t  RAW  tx=%02h%s %02h%s   rx=%02h%s %02h%s", $time,
               pipe_mac.tx[0].data[7:0],  pipe_mac.tx[0].char_is_k[0] ? "K" : "d",
               pipe_mac.tx[0].data[15:8], pipe_mac.tx[0].char_is_k[1] ? "K" : "d",
               ep_tx_data[7:0],       ep_tx_datak[0] ? "K" : "d",
               ep_tx_data[15:8],      ep_tx_datak[1] ? "K" : "d");
      dump_n <= dump_n + 1;
    end
`endif

  //------------------------------------------------------------------
  // Progress reporting
  //------------------------------------------------------------------
  // The LTSSM state is the single most useful thing to watch when the link
  // does not come up, so every transition is printed.
`ifndef NO_LTSSM_TRACE
  logic [5:0] ltssm_q;   // logic: the hard macro's state is X before its reset

  // in reset it follows the state, unprinted
  always_ff @(posedge pipe_clk or negedge pipe_rst_n) begin
    ltssm_q <= pl_ltssm_state;

    if (pipe_rst_n && ltssm_q !== pl_ltssm_state) begin
      $display("%t  LTSSM  0x%02h -> 0x%02h %s", $time, ltssm_q, pl_ltssm_state,
               ltssm_name(pl_ltssm_state));
    end
  end

  function automatic string ltssm_name(input logic [5:0] s);
    case (s)
      6'h00: return "DETECT_QUIET";
      6'h01: return "DETECT_ACTIVE";
      6'h02: return "POLLING_ACTIVE";
      6'h03: return "POLLING_COMPLIANCE";
      6'h04: return "POLLING_CONFIGURATION";
      6'h05: return "CONFIG_LINKWIDTH_START";
      6'h06: return "CONFIG_LINKWIDTH_ACCEPT";
      6'h07: return "CONFIG_LANENUM_WAIT";
      6'h08: return "CONFIG_LANENUM_ACCEPT";
      6'h09: return "CONFIG_COMPLETE";
      6'h0A: return "CONFIG_IDLE";
      6'h0B: return "RECOVERY_RCVRLOCK";
      6'h0C: return "RECOVERY_SPEED";
      6'h0D: return "RECOVERY_RCVRCFG";
      6'h0E: return "RECOVERY_IDLE";
      6'h10: return "L0";
      6'h16: return "L0  (linkup)";
      default: return "";
    endcase
  endfunction
`endif

endmodule

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
