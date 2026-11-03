//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   Co-simulation stand-in for 2.rtl/0.common.opensource/src/pcie/pcie_core.sv
//   (make RC=model PHY=hm, tb_socket.sv): the Artix-7 RC model in the hard
//   macro's socket. Same module name and ports, so the file list picks this
//   file instead and the rest of pcie_top stays: clk_rst_gen with its PCLK
//   switch, and phy_top (models/phy_top.EP-PIPE-BFM.sv, the PHY the hard
//   macro has, with the pcieVHost endpoint behind it).
//
//        RC model     PIPExN__pcieVHost, EndPoint = 0, node 2 (VUserMain2.cpp)
//          |          symbols
//        u_mac        pipe_mac_adapter: the MAC side of the PIPE
//          |          one register each way, as pcie_core has
//          |          pipe_mac, 16-bit, x1 or x4
//        phy_top      pipe_phy_core and the endpoint, as for the hard macro
//
//   PCLK and the rate: the model asks for Gen2 (its GEN2_CLK register), the
//   adapter changes Rate, phy_top selects clk_rst_gen's 250 MHz PCLK, and the
//   model's symbol clock follows Rate, as the endpoint's does. The adapter
//   leaves reset with the PHY ready (phy_rdy_n low).
//
//   The user side is idle: no TRN link up (the SOC stays in reset), no TLPs,
//   no credits. pl_ltssm_state stays DETECT_QUIET: the model's LTSSM is in
//   its C code, which logs its states (the Makefile turns phy_top's LTSSM
//   trace off).
//==========================================================================

`timescale 1ps/1ps

module pcie_core
  import link_pkg::*;
  import pcie_pkg::*;
(
    input  logic core_rst_n,
    output logic trn_link_up,

    axis_if.slave  s_axis_tx,
    axis_if.master m_axis_rx,

    output logic [5:0] tx_buf_avail,

    input logic [15:0] cfg_dev_id,
    input logic [15:0] cfg_vend_id,
    input logic [ 7:0] cfg_rev_id,
    input logic [15:0] cfg_subsys_id,
    input logic [15:0] cfg_subsys_vend_id,

    output logic cfg_msg_received_err_fatal,

    output logic [5:0] pl_ltssm_state,

          pipe_if.mac pipe_mac,
    input logic       phy_rdy_n,
    input logic       pipe_clk,
    input logic       user_clk
);

  localparam int L = PCIE_LANES;

  //------------------------------------------------------------------
  // The user side: idle
  //------------------------------------------------------------------
  assign trn_link_up                = 1'b0;
  assign tx_buf_avail               = '0;
  assign cfg_msg_received_err_fatal = 1'b0;

  assign s_axis_tx.tready = 1'b0;
  assign m_axis_rx.tdata  = '0;
  assign m_axis_rx.tkeep  = '0;
  assign m_axis_rx.tuser  = '0;
  assign m_axis_rx.tlast  = 1'b0;
  assign m_axis_rx.tvalid = 1'b0;

  //------------------------------------------------------------------
  // The RC model and the MAC side of the PIPE
  //------------------------------------------------------------------
  logic            rst_n;
  bit              rate_sel;    // Rate as a hard 0/1 for the symbol clock
  logic            rc_gen2;
  logic [16*L-1:0] rc_tx_data,  rc_rx_data,  mac_tx_data,  mac_rx_data;
  logic [ 2*L-1:0] rc_tx_datak, rc_rx_datak, mac_tx_datak, mac_rx_datak;
  logic [   L-1:0] mac_tx_elec_idle, mac_rx_valid, mac_rx_elec_idle;
  logic [     1:0] powerdown;
  pipe_tx_ctrl_t   tx_ctrl_d, tx_ctrl_q;
  pipe_tx_lane_t   [L-1:0] tx_d, tx_q;
  pipe_rx_lane_t   [L-1:0] rx_q;

  assign rst_n = (phy_rdy_n === 1'b0);

  initial rate_sel = 1'b0;
  always @(pipe_mac.tx_ctrl.rate) rate_sel = (pipe_mac.tx_ctrl.rate === 1'b1);

  PIPExN__pcieVHost #(
    .NodeNum   (2),      // VUserMain2.cpp -- the Artix-7 RC model
    .EndPoint  (0),
    .Lanes     (L),
    .DataWidth (16)
  ) u_rc (
    .pclk      (pipe_clk),
    .gen2      (rate_sel),
    .nreset    (rst_n),
    .Gen2ClkSel(rc_gen2),
    .RxData    (rc_rx_data),
    .RxDataK   (rc_rx_datak),
    .TxData    (rc_tx_data),
    .TxDataK   (rc_tx_datak),
    .dll_clk   (pipe_clk),
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

  pipe_mac_adapter #(
    .LANES (L),
    .BYTES (2)
  ) u_mac (
    .pclk         (pipe_clk),
    .rst_n        (rst_n),
    .link_up      (),
    .vh_gen2      (rc_gen2),
    .vh_txdata    (rc_tx_data),
    .vh_txdatak   (rc_tx_datak),
    .vh_rxdata    (rc_rx_data),
    .vh_rxdatak   (rc_rx_datak),
    .mac_reset    (tx_ctrl_d.reset),
    .powerdown    (powerdown),
    .rate         (tx_ctrl_d.rate),
    .tx_detect_rx (tx_ctrl_d.rcvr_det),
    .tx_elec_idle (mac_tx_elec_idle),
    .tx_data      (mac_tx_data),
    .tx_datak     (mac_tx_datak),
    .phy_status   (rx_q[0].phy_status),
    .rx_status    (rx_q[0].status),
    .rx_valid     (mac_rx_valid),
    .rx_elec_idle (mac_rx_elec_idle),
    .rx_data      (mac_rx_data),
    .rx_datak     (mac_rx_datak)
  );

  assign tx_ctrl_d.deemph = 1'b1;     // the reset value; the PHY model ignores it
  assign tx_ctrl_d.margin = 3'b000;

  for (genvar l = 0; l < L; l++) begin : g_lane
    assign tx_d[l].data       = mac_tx_data [16*l +: 16];
    assign tx_d[l].char_is_k  = mac_tx_datak[ 2*l +:  2];
    assign tx_d[l].elec_idle  = mac_tx_elec_idle[l];
    assign tx_d[l].compliance = 1'b0;
    assign tx_d[l].powerdown  = powerdown;

    assign mac_rx_data [16*l +: 16] = rx_q[l].data;
    assign mac_rx_datak[ 2*l +:  2] = rx_q[l].char_is_k;
    assign mac_rx_valid[l]          = rx_q[l].valid;
    assign mac_rx_elec_idle[l]      = rx_q[l].elec_idle;
  end

  // the PIPE registers of pcie_core, one PCLK each way, held while the PHY is
  // not ready
  always_ff @(posedge pipe_clk) begin
    if (phy_rdy_n !== 1'b0) begin
      tx_ctrl_q <= PIPE_TX_CTRL_RST;
      for (int l = 0; l < L; l++) begin
        tx_q[l] <= PIPE_TX_LANE_RST;
        rx_q[l] <= PIPE_RX_LANE_RST;
      end
    end
    else begin
      tx_ctrl_q <= tx_ctrl_d;
      tx_q      <= tx_d;
      rx_q      <= pipe_mac.rx;
    end
  end

  assign pipe_mac.tx_ctrl     = tx_ctrl_q;
  assign pipe_mac.tx          = tx_q;
  assign pipe_mac.rx_polarity = '0;

  assign pl_ltssm_state = 6'h00;

endmodule: pcie_core

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 2, 2026
// -----------------------------------------------------------------------------
