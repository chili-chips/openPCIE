//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   pipe_mac_adapter: the MAC side of a PIPE, for a pcieVHost model. It is
//   the mirror of pipe_phy_core: there the model is the PHY's link partner,
//   here it is the MAC above a PHY. The model has only symbols ({K, byte},
//   NOSIG for electrical idle); the adapter adds what a PIPE MAC does with
//   the PHY's side-band signals:
//     1. PIPE reset: Reset high RST_CYC PCLK, in P1 at Gen1; then it waits
//        for PhyStatus low (the PHY ready)
//     2. receiver detection in P1: TxDetectRx high until the PhyStatus
//        pulse; RxStatus 011 is a receiver. None: DET_QUIET PCLK in P1,
//        then again (Detect.Quiet, shortened)
//     3. P0: requested after a receiver is found, granted by the PhyStatus
//        pulse
//     4. L0 (P0 granted): the model's symbols both ways. TX: TxElecIdle
//        per lane while the model sends NOSIG on every symbol of the word;
//        a NOSIG symbol in a word that is not all idle goes out as D0.0
//        (PIPE has no idle per symbol). RX: the PHY's data while RxValid
//        and not RxElecIdle, NOSIG otherwise
//     5. rate change: Rate follows the model's Gen2 clock request; TX idle
//        and RX NOSIG until the PhyStatus pulse that answers it
//   Until L0 the model receives NOSIG and its TX is held idle. One bring-up
//   per reset: a model back in Detect is not seen. One PhyStatus, RxStatus
//   and TxDetectRx for all lanes, as pipe_phy_core has.
//
//   Assertions end the run: on the adapter's own PIPE rules, and on the
//   PHY's answers (PhyStatus within PHYSTATUS_TO PCLK, never unasked, no X
//   out of Reset).
//
//   Master: openPCIE 5.sim/models.
//==========================================================================

`timescale 1ps/1ps

module pipe_mac_adapter
  import pipe_sym_pkg::*;
#(
    parameter int LANES         = 1,
    parameter int BYTES         = 2,    // symbols per lane per PCLK
    parameter int RST_CYC       = 4,    // PCLK of PIPE Reset
    parameter int DET_QUIET     = 64,   // PCLK in P1 between two detections
    parameter int PHYSTATUS_TO  = 10000 // PCLK the PHY may take to answer
) (
    input  logic                     pclk,
    input  logic                     rst_n,
    output logic                     link_up,       // P0 granted: L0 or a rate change

    // the model (pcieVHost)
    input  logic                     vh_gen2,       // its Gen2 clock request
    input  logic [LANES*BYTES*8-1:0] vh_txdata,     // model -> PHY
    input  logic [  LANES*BYTES-1:0] vh_txdatak,
    output logic [LANES*BYTES*8-1:0] vh_rxdata,     // PHY -> model
    output logic [  LANES*BYTES-1:0] vh_rxdatak,

    // MAC -> PHY
    output logic                     mac_reset,     // PIPE Reset: 1 = in reset
    output logic [              1:0] powerdown,
    output logic                     rate,          // 1 = 5.0 GT/s
    output logic                     tx_detect_rx,
    output logic [        LANES-1:0] tx_elec_idle,
    output logic [LANES*BYTES*8-1:0] tx_data,
    output logic [  LANES*BYTES-1:0] tx_datak,

    // PHY -> MAC
    input  logic                     phy_status,
    input  logic [              2:0] rx_status,
    input  logic [        LANES-1:0] rx_valid,
    input  logic [        LANES-1:0] rx_elec_idle,
    input  logic [LANES*BYTES*8-1:0] rx_data,
    input  logic [  LANES*BYTES-1:0] rx_datak
);

  localparam bit   [1:0] P0        = 2'b00;
  localparam bit   [1:0] P1        = 2'b10;

  //------------------------------------------------------------------
  // Bring-up and rate change
  //------------------------------------------------------------------
  typedef enum bit [2:0] {RESET, PHY_RDY, DETECT, QUIET, TO_P0, L0, RATE} state_t;

  state_t state;
  int     cnt;

  always_ff @(posedge pclk or negedge rst_n) begin
    if (!rst_n) begin
      state        <= RESET;
      cnt          <= RST_CYC;
      mac_reset    <= 1'b1;
      powerdown    <= P1;
      rate         <= 1'b0;
      tx_detect_rx <= 1'b0;
    end
    else begin
      case (state)
        RESET: begin
          if (cnt > 1) begin
            cnt <= cnt - 1;
          end
          else begin
            mac_reset <= 1'b0;
            state     <= PHY_RDY;
          end
        end
        PHY_RDY: begin
          if (phy_status === 1'b0) begin
            tx_detect_rx <= 1'b1;
            state        <= DETECT;
          end
        end
        DETECT: begin
          if (phy_status === 1'b1) begin
            tx_detect_rx <= 1'b0;
            if (rx_status == 3'b011) begin
              powerdown <= P0;
              state     <= TO_P0;
            end
            else begin
              cnt   <= DET_QUIET;
              state <= QUIET;
            end
          end
        end
        QUIET: begin
          if (cnt > 1) begin
            cnt <= cnt - 1;
          end
          else begin
            tx_detect_rx <= 1'b1;
            state        <= DETECT;
          end
        end
        TO_P0: begin
          if (phy_status === 1'b1) state <= L0;
        end
        L0: begin
          if ((vh_gen2 === 1'b1) != rate) begin
            rate  <= !rate;
            state <= RATE;
          end
        end
        default: begin   // RATE
          if (phy_status === 1'b1) state <= L0;
        end
      endcase
    end
  end

  assign link_up = (state == L0) || (state == RATE);

  // PCLK the adapter has waited for a PhyStatus
  int  wait_cnt;
  bit  waiting;

  assign waiting = (state inside {PHY_RDY, DETECT, TO_P0, RATE});

  always_ff @(posedge pclk or negedge rst_n) begin
    if (!rst_n)       wait_cnt <= 0;
    else if (waiting) wait_cnt <= wait_cnt + 1;
    else              wait_cnt <= 0;
  end

  //------------------------------------------------------------------
  // Symbols
  //------------------------------------------------------------------
  for (genvar l = 0; l < LANES; l++) begin : g_lane
    logic [BYTES-1:0] tx_nosig;
    logic             rx_ok;

    assign rx_ok           = (state == L0) && (rx_valid[l] === 1'b1) && (rx_elec_idle[l] === 1'b0);
    assign tx_elec_idle[l] = (state != L0) || (&tx_nosig);

    for (genvar b = 0; b < BYTES; b++) begin : g_b
      localparam int S = BYTES*l + b;

      assign tx_nosig[b]         = ({vh_txdatak[S], vh_txdata[8*S +: 8]} === NOSIG_SYM);
      assign tx_data [8*S +: 8]  = tx_nosig[b] ? 8'h00 : vh_txdata[8*S +: 8];
      assign tx_datak[S]         = tx_nosig[b] ? 1'b0  : vh_txdatak[S];

      assign vh_rxdata [8*S +: 8] = rx_ok ? rx_data[8*S +: 8] : NOSIG_SYM[7:0];
      assign vh_rxdatak[S]        = rx_ok ? rx_datak[S]       : NOSIG_SYM[8];
    end
  end

  //------------------------------------------------------------------
  // The PIPE rules the adapter keeps (PIPE 3.0, Base 2.1 4.2.6.1)
  //------------------------------------------------------------------
  // receiver detection only in P1, with the transmitter idle, and held until
  // PhyStatus answers it
  a_det_p1: assert property (@(posedge pclk) disable iff (!rst_n)
      tx_detect_rx |-> (powerdown == P1) && (&tx_elec_idle))
    else $fatal(1, "pipe_mac_adapter: TxDetectRx outside P1 or with TX active");

  a_det_held: assert property (@(posedge pclk) disable iff (!rst_n)
      $fell(tx_detect_rx) |-> $past(phy_status === 1'b1))
    else $fatal(1, "pipe_mac_adapter: TxDetectRx dropped before PhyStatus");

  // P0 only after a receiver was found
  a_p0_found: assert property (@(posedge pclk) disable iff (!rst_n)
      $rose(powerdown == P0) |-> $past(rx_status == 3'b011 && phy_status))
    else $fatal(1, "pipe_mac_adapter: P0 without a receiver");

  // data only in P0; a rate change only when the model was idle
  a_tx_p0: assert property (@(posedge pclk) disable iff (!rst_n)
      !(&tx_elec_idle) |-> (powerdown == P0))
    else $fatal(1, "pipe_mac_adapter: TX active outside P0");

  // the PHY: it answers, only when asked, and drives no X out of Reset
  a_phy_answers: assert property (@(posedge pclk) disable iff (!rst_n)
      wait_cnt < PHYSTATUS_TO)
    else $fatal(1, "pipe_mac_adapter: no PhyStatus in %0d PCLK (state %s)", PHYSTATUS_TO,
                state.name());

  a_phy_asked: assert property (@(posedge pclk) disable iff (!rst_n)
      (phy_status === 1'b1) |-> (state inside {RESET, PHY_RDY, DETECT, TO_P0, RATE}))
    else $fatal(1, "pipe_mac_adapter: PhyStatus unasked (state %s)", state.name());

  a_phy_known: assert property (@(posedge pclk) disable iff (!rst_n)
      !mac_reset |-> !$isunknown({phy_status, rx_status}))
    else $fatal(1, "pipe_mac_adapter: PhyStatus or RxStatus unknown");

  a_rate_idle: assert property (@(posedge pclk) disable iff (!rst_n)
      $changed(rate) |-> $past(&tx_elec_idle))
    else $fatal(1, "pipe_mac_adapter: Rate change asked with TX active");

endmodule: pipe_mac_adapter

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 2, 2026
// -----------------------------------------------------------------------------
