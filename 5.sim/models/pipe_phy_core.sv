//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   pipe_phy_core: the PHY side of a PIPE, for a link partner that is a
//   pcieVHost model on the other side. One core for both projects, plain
//   ports; each wraps it for its own pipe_if:
//     openPCIE         models/phy_top.EP-PIPE-BFM.sv  (PCIE_2_1 hard macro)
//     openCologne-PCIE models/phy_vproc_rc.sv         (our MAC)
//   Master: openPCIE 5.sim/models; the copy in openCologne-PCIE is identical.
//
//   The PIPE handshakes, as a PHY gives them (the GateMate PHY's
//   ccfpga_pipe_fsm does the same):
//     - PhyStatus high in PIPE reset and RST_LAT - 1 PCLK after it (RST_LAT = 0:
//       low throughout)
//     - one PhyStatus pulse per change of PowerDown or Rate, PHYSTATUS_LAT
//       PCLK after the request
//     - receiver detection in P1: TxDetectRx high, RCVRDET_LAT PCLK later one
//       PhyStatus pulse with RxStatus 011 (receiver present) or 000 (none);
//       TxDetectRx dropped before that aborts it
//
//   Electrical idle is NOSIG (the K flag and 0xFF: no 8b/10b code) per symbol
//   on the partner's side. The partner gets NOSIG under TxElecIdle, never the
//   data the MAC leaves there. The MAC gets 0 for each NOSIG symbol, so a word
//   half idle (idle entry after an EIOS, exit before a TS) carries no K + 0xFF.
//   Per lane, RxValid from the first K symbol after idle (symbol lock on the
//   COM that opens training) until the next all-NOSIG word, and RxElecIdle
//   otherwise: a squelch that ignores what a partner drives before it trains
//   (a model out of reset sends D0.0).
//==========================================================================

`timescale 1ps/1ps

`include "pciexpress_header.v"

module pipe_phy_core #(
    parameter int LANES         = 1,
    parameter int BYTES         = 2,    // symbols per lane per PCLK
    parameter bit TX_FLOP       = 0,    // 1: the partner gets the MAC's TX one PCLK later
    parameter int RDY_LAT       = 16,   // PCLK from reset to phy_rdy
    parameter int RST_LAT       = 8,    // PhyStatus high RST_LAT - 1 PCLK after PIPE reset; 0: never
    parameter int PHYSTATUS_LAT = 8,    // PCLK to a PhyStatus answer
    parameter int RCVRDET_LAT   = 16    // PCLK to the receiver-detection result
) (
    input  logic                     pclk,
    input  logic                     rst_n,
    output logic                     phy_rdy,

    // MAC -> PHY
    input  logic                     mac_reset,     // PIPE Reset: 1 = in reset
    input  logic [              1:0] powerdown,     // P0, P0s, P1, P2
    input  logic                     rate,          // 1 = 5.0 GT/s
    input  logic                     tx_detect_rx,
    input  logic [        LANES-1:0] tx_elec_idle,
    input  logic [LANES*BYTES*8-1:0] tx_data,
    input  logic [  LANES*BYTES-1:0] tx_datak,

    // PHY -> MAC
    output logic                     phy_status,
    output logic [              2:0] rx_status,
    output logic [        LANES-1:0] rx_valid,
    output logic [        LANES-1:0] rx_elec_idle,
    output logic [LANES*BYTES*8-1:0] rx_data,
    output logic [  LANES*BYTES-1:0] rx_datak,

    // the link partner
    input  logic                     rcvr_present,  // receiver detection finds it
    output logic [LANES*BYTES*8-1:0] host_rxdata,   // MAC TX -> partner
    output logic [  LANES*BYTES-1:0] host_rxdatak,
    input  logic [LANES*BYTES*8-1:0] host_txdata,   // partner -> MAC RX
    input  logic [  LANES*BYTES-1:0] host_txdatak
);

  localparam logic [8:0] NOSIG_SYM = {1'b1, `NOSIG};   // {K, byte}
  localparam bit   [1:0] P1        = 2'b10;

  //------------------------------------------------------------------
  // PHY ready
  //------------------------------------------------------------------
  int rdy_cnt;

  always_ff @(posedge pclk or negedge rst_n) begin
    if (!rst_n) begin
      rdy_cnt <= 0;
    end
    else if (rdy_cnt != RDY_LAT) begin
      rdy_cnt <= rdy_cnt + 1;
    end
  end

  assign phy_rdy = (rdy_cnt == RDY_LAT);

  //------------------------------------------------------------------
  // PhyStatus and RxStatus
  //------------------------------------------------------------------
  // The PHY's own power state and rate follow the MAC's request when it
  // answers; a request that differs from them starts an answer. A request
  // with an unknown bit (the MAC still in reset) starts nothing
  typedef enum bit [1:0] {NONE, RST, CHG, DET} job_t;

  job_t     job;
  int       cnt;          // PCLK left to the answer
  bit [1:0] pd_phy, pd_req;
  bit       rate_phy, rate_req;
  bit       det_done;     // detection answered, until TxDetectRx drops
  bit       status_r;
  bit [2:0] rx_status_r;

  always_ff @(posedge pclk or negedge rst_n) begin
    if (!rst_n) begin
      job         <= RST;
      cnt         <= RST_LAT;
      pd_phy      <= P1;
      pd_req      <= P1;
      rate_phy    <= 1'b0;
      rate_req    <= 1'b0;
      det_done    <= 1'b0;
      status_r    <= (RST_LAT != 0);
      rx_status_r <= 3'b000;
    end
    else begin
      status_r    <= 1'b0;
      rx_status_r <= 3'b000;

      if (!tx_detect_rx) det_done <= 1'b0;

      // PIPE reset: PhyStatus high, the PHY back in P1 at Gen1
      if (mac_reset === 1'b1) begin
        job      <= RST;
        cnt      <= RST_LAT;
        pd_phy   <= P1;
        rate_phy <= 1'b0;
        status_r <= (RST_LAT != 0);
      end
      else if (job == RST) begin
        if (cnt > 1) begin
          cnt      <= cnt - 1;
          status_r <= 1'b1;
        end
        else begin
          job <= NONE;
        end
      end
      // the MAC gave up on detection
      else if (job == DET && tx_detect_rx !== 1'b1) begin
        job <= NONE;
      end
      else if (job != NONE) begin
        if (cnt > 1) begin
          cnt <= cnt - 1;
        end
        else begin
          job      <= NONE;
          status_r <= 1'b1;
          if (job == DET) begin
            det_done    <= 1'b1;
            rx_status_r <= rcvr_present ? 3'b011 : 3'b000;
          end
          else begin
            pd_phy   <= pd_req;
            rate_phy <= rate_req;
          end
        end
      end
      else if (powerdown == P1 && pd_phy == P1 && tx_detect_rx === 1'b1 && !det_done) begin
        job <= DET;
        cnt <= RCVRDET_LAT;
      end
      else if (!$isunknown({powerdown, rate}) && (powerdown != pd_phy || rate != rate_phy)) begin
        job      <= CHG;
        cnt      <= PHYSTATUS_LAT;
        pd_req   <= powerdown;
        rate_req <= rate;
      end
    end
  end

  assign phy_status = status_r;
  assign rx_status  = rx_status_r;

  //------------------------------------------------------------------
  // Transmit: MAC -> partner
  //------------------------------------------------------------------
  // TxElecIdle unknown (the MAC still in reset) is idle too: no X reaches the
  // partner from there
  logic [LANES*BYTES*8-1:0] tx_sym;
  logic [  LANES*BYTES-1:0] tx_symk;

  always_comb begin
    for (int l = 0; l < LANES; l++) begin
      for (int b = 0; b < BYTES; b++) begin
        tx_sym [8*(BYTES*l+b) +: 8] = (tx_elec_idle[l] !== 1'b0) ? NOSIG_SYM[7:0] : tx_data [8*(BYTES*l+b) +: 8];
        tx_symk[   BYTES*l+b]       = (tx_elec_idle[l] !== 1'b0) ? NOSIG_SYM[8]   : tx_datak[   BYTES*l+b];
      end
    end
  end

  if (TX_FLOP) begin : g_tx_flop
    always_ff @(posedge pclk or negedge rst_n) begin
      if (!rst_n) begin
        host_rxdata  <= {LANES*BYTES{NOSIG_SYM[7:0]}};   // idle in reset
        host_rxdatak <= '1;
      end
      else begin
        host_rxdata  <= tx_sym;
        host_rxdatak <= tx_symk;
      end
    end
  end
  else begin : g_tx_comb
    assign host_rxdata  = tx_sym;
    assign host_rxdatak = tx_symk;
  end

  //------------------------------------------------------------------
  // Receive: partner -> MAC
  //------------------------------------------------------------------
  // NOSIG only (===): an X from the partner is a bug, and must reach the MAC
  for (genvar l = 0; l < LANES; l++) begin : g_rx
    logic [BYTES-1:0] nosig;
    logic             idle;     // the whole word
    logic             has_k;
    bit               locked;   // a K seen since the last idle word

    for (genvar b = 0; b < BYTES; b++) begin : g_b
      localparam int S = BYTES*l + b;

      assign nosig[b]          = ({host_txdatak[S], host_txdata[8*S +: 8]} === NOSIG_SYM);
      assign rx_data [8*S +: 8] = nosig[b] ? 8'h00 : host_txdata[8*S +: 8];
      assign rx_datak[S]        = nosig[b] ? 1'b0  : host_txdatak[S];
    end

    assign idle  = &nosig;
    assign has_k = ((|rx_datak[BYTES*l +: BYTES]) === 1'b1);

    always_ff @(posedge pclk or negedge rst_n) begin
      if (!rst_n) begin
        locked <= 1'b0;
      end
      else if (idle) begin
        locked <= 1'b0;
      end
      else if (has_k) begin
        locked <= 1'b1;
      end
    end

    assign rx_valid[l]     = !idle && (locked || has_k);
    assign rx_elec_idle[l] = !rx_valid[l];
  end

endmodule: pipe_phy_core

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 2, 2026
// -----------------------------------------------------------------------------
