//==========================================================================
// Copyright (c) 2025 - 2026 Simon Southwell (pcieVHostPipex1, which this extends)
// Copyright (C) 2026 Chili.CHIPS (the N-lane version)
//--------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//   Derived from pcievhost's pcieVHostPipex1.v, so GPL-3.0-or-later like it.
//--------------------------------------------------------------------------
// Description:
//   pcieVHostPipexN: upstream's pcieVHostPipex1 (pcievhost
//   verilog/pcieVHost/pcieVHostPipex1.v) for Lanes lanes: the same PIPE
//   shifters, one set per lane, around one PcieVhost with LinkWidth = Lanes.
//   8b/10b is off, so each lane carries {K, byte} symbols, as in the x1
//   wrapper. Lane l is bits [l*DataWidth +: DataWidth] of RxData/TxData.
//
//   Differences from pcieVHostPipex1: no clock switch inside (Gen2Clk = 0, as
//   the test bench uses the x1 wrapper too): the test bench gives the symbol
//   clock for the rate, 250 or 500 MHz. Electrical idle is NOSIG
//   (the K flag and 0xFF: no 8b/10b code) per symbol, as there, and reaches
//   the core on its ElecIdleIn, one bit per lane.
//==========================================================================

`include "pciexpress_header.svh"

module pcieVHostPipexN
  import pipe_sym_pkg::*;
#(
    parameter int NodeNum   = 8,
    parameter bit EndPoint  = 1,
    parameter int Lanes     = 4,   // 1 to 16
    parameter int DataWidth = 16   // 8, 16, 32 or 64, per lane
) (
    input  logic                             pcieclk,  // symbol clock: 250 MHz Gen1, 500 MHz Gen2
    input  logic                             pclk,     // pcieclk / (DataWidth/8), synchronous
    input  logic                             nreset,
    output logic                             Gen2ClkSel,
    output logic                             ClkOut,
    input  logic [Lanes*DataWidth    -1:0]   RxData,
    input  logic [Lanes*DataWidth/8  -1:0]   RxDataK,
    output bit   [Lanes*DataWidth    -1:0]   TxData,
    output bit   [Lanes*DataWidth/8  -1:0]   TxDataK,

    // Transaction taps, on ClkOut: see pcieVHost.sv
    output logic [31:0]                      TlpTxData,
    output logic                             TlpTxSop, TlpTxEop, TlpTxValid,
    output logic [31:0]                      TlpRxData,
    output logic                             TlpRxSop, TlpRxEop, TlpRxValid,
    output logic [31:0]                      DllpTxData,
    output logic                             DllpTxValid,
    output logic [31:0]                      DllpRxData,
    output logic                             DllpRxValid
);

  localparam int BYTES = DataWidth / 8;

  // one 10-bit link symbol per lane, {0, K, byte}; lanes above Lanes are 0 and idle
  logic [15:0][9:0] link_in, link_out;

  bit   [Lanes-1:0][DataWidth-1:0] rx_sample, tx_pipe;
  bit   [Lanes-1:0][   BYTES-1:0]  rxk_sample, txk_pipe;
  int                              rxcount;

  // electrical idle is NOSIG per symbol, both ways: the core's idle (its
  // ElecIdleOut) goes out as NOSIG, and a received NOSIG is the partner's
  // idle, to the core's ElecIdleIn
  logic [15:0]      link_out_idle;
  bit   [15:0][7:0] link_out_q;     // the core's TX symbols, on the falling edge
  bit   [15:0]      link_outk_q;
  logic [15:0]      link_in_idle;
  logic [Lanes-1:0] rx_idle;

  // pipe_sym_pkg's NOSIG_SYM is the patch's `NOSIG (the header is included
  // for this check only)
  initial begin
    if (NOSIG_SYM != {1'b1, `NOSIG}) begin
      $fatal(1, "pipe_sym_pkg::NOSIG_SYM differs from NOSIG in pciexpress_header.svh");
    end
  end

  for (genvar l = 0; l < 16; l++) begin : g_link_in
    if (l < Lanes) begin : g_used
      assign rx_idle[l]      = ({rxk_sample[l][0], rx_sample[l][7:0]} == NOSIG_SYM);
      assign link_in_idle[l] = rx_idle[l];
      assign link_in[l]      = {1'b0, rxk_sample[l][0], rx_sample[l][7:0]};
    end else begin : g_unused
      assign link_in_idle[l] = 1'b1;
      assign link_in[l]      = '0;
    end
  end

  // Sample each lane's RX word every BYTES symbol clocks and shift it out one
  // symbol per clock; shift the link's TX symbols into each lane's word
  // RX on the falling edge: the core reads LinkIn and ElecIdleIn on the rising
  // edge, in VProc's delta cycles, so a sample changing on that edge was seen
  // before or after the read as the scheduler chose (pcieVHostPipex1).
  // Every register resets on nreset, asynchronously: in reset the line is
  // idle (NOSIG with the K flag), both ways
  always_ff @(negedge pcieclk or negedge nreset) begin
    if (!nreset) begin
      rxcount    <= 0;
      rx_sample  <= {Lanes*BYTES{NOSIG_SYM[7:0]}};
      rxk_sample <= '1;
    end else if (rxcount == 0) begin
      for (int l = 0; l < Lanes; l++) begin
        rx_sample[l]  <= RxData [l*DataWidth +: DataWidth];
        rxk_sample[l] <= RxDataK[l*BYTES     +: BYTES];
      end
      rxcount <= BYTES - 1;
    end else begin
      for (int l = 0; l < Lanes; l++) begin
        rx_sample[l]  <= DataWidth'(rx_sample[l]  >> 8);
        rxk_sample[l] <= BYTES'(rxk_sample[l] >> 1);
      end
      rxcount <= rxcount - 1;
    end
  end

  // the core's TX symbols on the falling edge: it writes them on the rising
  // edge, in VProc's delta cycles, the edge the TX shift runs on (pcieVHostPipex1)
  always_ff @(negedge pcieclk or negedge nreset) begin
    if (!nreset) begin
      link_out_q  <= {16{NOSIG_SYM[7:0]}};
      link_outk_q <= '1;
    end else for (int l = 0; l < 16; l++) begin
      link_out_q[l]  <= link_out_idle[l] ? NOSIG_SYM[7:0] : link_out[l][7:0];
      link_outk_q[l] <= link_out_idle[l] ? NOSIG_SYM[8]   : link_out[l][8];
    end
  end

  if (BYTES == 1) begin : g_tx1
    always_ff @(posedge pcieclk or negedge nreset) begin
      if (!nreset) begin
        tx_pipe  <= {Lanes*BYTES{NOSIG_SYM[7:0]}};
        txk_pipe <= '1;
      end else for (int l = 0; l < Lanes; l++) begin
        tx_pipe[l]  <= DataWidth'(link_out_q[l]);
        txk_pipe[l] <= BYTES'(link_outk_q[l]);
      end
    end
  end else begin : g_txn
    always_ff @(posedge pcieclk or negedge nreset) begin
      if (!nreset) begin
        tx_pipe  <= {Lanes*BYTES{NOSIG_SYM[7:0]}};
        txk_pipe <= '1;
      end else for (int l = 0; l < Lanes; l++) begin
        tx_pipe[l]  <= {link_out_q[l], tx_pipe[l][DataWidth-1:8]};
        txk_pipe[l] <= {link_outk_q[l], txk_pipe[l][BYTES-1:1]};
      end
    end
  end

  // At the PIPE clock rate, update the TX outputs (pcieclk when 8 bits wide)
  logic tx_sample_clk;
  assign tx_sample_clk = (BYTES == 1) ? pcieclk : pclk;

  always_ff @(posedge tx_sample_clk or negedge nreset) begin
    if (!nreset) begin
      TxData  <= {Lanes*BYTES{NOSIG_SYM[7:0]}};
      TxDataK <= '1;
    end else for (int l = 0; l < Lanes; l++) begin
      TxData [l*DataWidth +: DataWidth] <= tx_pipe[l];
      TxDataK[l*BYTES     +: BYTES]     <= txk_pipe[l];
    end
  end

  PcieVhost #(
      .LinkWidth        (Lanes),
      .NodeNum          (NodeNum),
      .EndPoint         (EndPoint),
      .DisableScrambling(0),
      .Disable8b10b     (1),
      .Gen2Clk          (0)
  ) pcievh_i (
      .Clk        (pcieclk),
      .notReset   (nreset),
      .Gen2ClkSel (Gen2ClkSel),
      .ClkOut     (ClkOut),
      .ElecIdleOut(link_out_idle),
      .ElecIdleIn (link_in_idle),
      .LinkIn0    (link_in[0]),  .LinkIn1  (link_in[1]),  .LinkIn2  (link_in[2]),  .LinkIn3  (link_in[3]),
      .LinkIn4    (link_in[4]),  .LinkIn5  (link_in[5]),  .LinkIn6  (link_in[6]),  .LinkIn7  (link_in[7]),
      .LinkIn8    (link_in[8]),  .LinkIn9  (link_in[9]),  .LinkIn10 (link_in[10]), .LinkIn11 (link_in[11]),
      .LinkIn12   (link_in[12]), .LinkIn13 (link_in[13]), .LinkIn14 (link_in[14]), .LinkIn15 (link_in[15]),
      .LinkOut0   (link_out[0]), .LinkOut1 (link_out[1]), .LinkOut2 (link_out[2]), .LinkOut3 (link_out[3]),
      .LinkOut4   (link_out[4]), .LinkOut5 (link_out[5]), .LinkOut6 (link_out[6]), .LinkOut7 (link_out[7]),
      .LinkOut8   (link_out[8]), .LinkOut9 (link_out[9]), .LinkOut10(link_out[10]),.LinkOut11(link_out[11]),
      .LinkOut12  (link_out[12]),.LinkOut13(link_out[13]),.LinkOut14(link_out[14]),.LinkOut15(link_out[15]),
      .TlpTxData  (TlpTxData),   .TlpTxSop (TlpTxSop),    .TlpTxEop (TlpTxEop),    .TlpTxValid(TlpTxValid),
      .TlpRxData  (TlpRxData),   .TlpRxSop (TlpRxSop),    .TlpRxEop (TlpRxEop),    .TlpRxValid(TlpRxValid),
      .DllpTxData (DllpTxData),  .DllpTxValid(DllpTxValid),
      .DllpRxData (DllpRxData),  .DllpRxValid(DllpRxValid)
  );

endmodule: pcieVHostPipexN

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Oct 24, 2026
// -----------------------------------------------------------------------------
