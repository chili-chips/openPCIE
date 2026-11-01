//=============================================================
//
// Copyright (c) 2025 - 2026 Simon Southwell. All rights reserved.
//
// Date: 25th July 2025
//
// This file is part of the pcieVHost package.
//
// The code is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This code is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this code. If not, see <http://www.gnu.org/licenses/>.
//
//=============================================================

`ifdef VPROC_SV
`include "allheaders.v"
`endif

`WsTimeScale

//-------------------------------------------------------------
// pcieVhostPipex1 module
//-------------------------------------------------------------

module pcieVHostPipex1
#(parameter NodeNum   = 8,
            EndPoint  = 1,
            DataWidth = 8, // 8, 16, 32 or 64 only
            Gen2Clk   = 0  // When non-zero, clocks must be at GEN2 speeds
)
(
  input                          pcieclk, // For GEN1 = 250MHz, for GEN2 = 500MHz (if Gen2Clk parameter = 1)
  input                          pclk,    // pcieclk /(DataWidth/8), synchronous
  input                          nreset,

  output                         Gen2ClkSel,
  output                         ClkOut,

  // openPCIE: electrical idle is NOSIG (the K flag and 0xFF: no 8b/10b code)
  // on the PIPE data, per symbol, on every simulator. Upstream had Z on a
  // 4-state simulator and a side-band under Verilator (ElecIdleOut/ElecIdleIn),
  // which reached the partner ahead of the data
  input  [DataWidth-1:0]         RxData,
  input  [(DataWidth/8)-1:0]     RxDataK,

  output bit [DataWidth-1:0]     TxData,
  output bit [(DataWidth/8)-1:0] TxDataK,

  // Transaction taps (openPCIE), on ClkOut: see pcieVHost.v
  output [31:0]                  TlpTxData,
  output                         TlpTxSop,  TlpTxEop,  TlpTxValid,
  output [31:0]                  TlpRxData,
  output                         TlpRxSop,  TlpRxEop,  TlpRxValid,
  output [31:0]                  DllpTxData,
  output                         DllpTxValid,
  output [31:0]                  DllpRxData,
  output                         DllpRxValid
);

//-------------------------------------------------------------
// Local parameter constant definitions
//-------------------------------------------------------------

localparam LINKWIDTH         = 1;

//-------------------------------------------------------------
// Signal declarations
//-------------------------------------------------------------

wire  [9:0]                  LinkOut;
wire  [9:0]                  LinkIn;
wire                         TxSampleClk;

wire  [DataWidth-1:0]        RxDataSampleShift;
wire  [DataWidth/8-1:0]      RxDataKSampleShift;
wire  [DataWidth-1:0]        TxDataPipeShift;
wire  [DataWidth/8-1:0]      TxDataKPipeShift;

wire                         pclk_main;
wire                         pcieclk_main;

bit                          pcieclk_div2;
bit                          pclk_div2;

bit   [DataWidth-1:0]        RxDataSample;
bit   [DataWidth/8-1:0]      RxDataKSample;
bit   [DataWidth-1:0]        TxDataPipe;
bit   [DataWidth/8-1:0]      TxDataKPipe;

int                          rxcount;

// openPCIE: the core's electrical idle (its ElecIdleOut) goes out as NOSIG; a
// received NOSIG is the partner's idle, to the core's ElecIdleIn
localparam [8:0]             NOSIG_SYM   = {1'b1, `NOSIG};   // {K, byte}
logic [15:0]                 ElecIdleOutInt;
logic                        LinkOutIdle;
logic [7:0]                  LinkOutData;
logic                        LinkOutK;
logic                        RxIdle;

assign LinkOutIdle           = ElecIdleOutInt[0];
assign LinkOutData           = LinkOutIdle ? NOSIG_SYM[7:0] : LinkOut[7:0];
assign LinkOutK              = LinkOutIdle ? NOSIG_SYM[8]   : LinkOut[8];
assign RxIdle                = ({RxDataKSample[0], RxDataSample[7:0]} == NOSIG_SYM);

// openPCIE: the core's TX symbol and idle, taken on the falling edge. The core
// writes them on the rising edge, in VProc's delta cycles, the edge the TX
// shift runs on, so which one the shift saw was the scheduler's choice (the TX
// side of issue 56). Half a symbol later both have settled.
// openPCIE: every register resets on nreset, asynchronously: in reset the line
// is idle (NOSIG with the K flag), both ways
bit   [7:0]                  LinkOutDataQ;
bit                          LinkOutKQ;

always_ff @(negedge pcieclk_main or negedge nreset)
begin
  if (nreset == 1'b0)
  begin
    LinkOutDataQ             <= `NOSIG;
    LinkOutKQ                <= 1'b1;
  end
  else
  begin
    LinkOutDataQ             <= LinkOutData;
    LinkOutKQ                <= LinkOutK;
  end
end

//-------------------------------------------------------------
// Select local clocks
//-------------------------------------------------------------

generate
  if (Gen2Clk == 0)
  begin
    assign pcieclk_main = pcieclk;
    assign pclk_main    = pclk;
  end
  else
  begin

    // openPCIE: always_ff, reset on nreset to upstream's start value, 0 (the
    // clock muxes hold both clocks off in reset)
    always_ff @(posedge pclk or negedge nreset)
    begin
      if (nreset == 1'b0)
        pclk_div2       <= 1'b0;
      else
        pclk_div2       <= ~pclk_div2;
    end

    always_ff @(posedge pcieclk or negedge nreset)
    begin
      if (nreset == 1'b0)
        pcieclk_div2    <= 1'b0;
      else
        pcieclk_div2    <= ~pcieclk_div2;
    end

    clkmux clkmux_pclk
    (
      .aresetn           (nreset),
      .clka              (pclk),
      .clkb              (pclk_div2),
      .sel               (Gen2ClkSel),
      .clkout            (pclk_main)
    );

    clkmux clkmux_pcieclk
    (
      .aresetn           (nreset),
      .clka              (pcieclk),
      .clkb              (pcieclk_div2),
      .sel               (Gen2ClkSel),
      .clkout            (pcieclk_main)
    );
  end
endgenerate

//-------------------------------------------------------------
// If DataWidth is 8 use the pcieclk and make the pclk unused
// (for backward compatibility)
//-------------------------------------------------------------

generate
  if (DataWidth == 8)
  begin
    assign TxSampleClk        = pcieclk_main;
    assign RxDataSampleShift  = RxData;
    assign RxDataKSampleShift = RxDataK;
    assign TxDataPipeShift    = LinkOutDataQ;
    assign TxDataKPipeShift   = LinkOutKQ;
  end
  else
  begin
    assign TxSampleClk        = pclk_main;
    assign RxDataSampleShift  = {8'h00, RxDataSample[DataWidth-1:8]};
    assign RxDataKSampleShift = {1'b0,  RxDataKSample[DataWidth/8-1: 1]};
    assign TxDataPipeShift    = {LinkOutDataQ, TxDataPipe[DataWidth-1:8]};
    assign TxDataKPipeShift   = {LinkOutKQ, TxDataKPipe[DataWidth/8-1:1]};
  end
endgenerate

// Link input is the lower bits of the sampled RX input
// openPCIE: the partner's electrical idle reaches the core on its ElecIdleIn
assign LinkIn                = {1'b0, RxDataKSample[0], RxDataSample[7:0]};

//-------------------------------------------------------------
// Process PIPE interface signals
//-------------------------------------------------------------

always_ff @(posedge pcieclk_main or negedge nreset)
begin

  if (nreset == 1'b0)
  begin
    // openPCIE: in reset the line is idle (NOSIG with the K flag). The TX
    // shift ran in reset too, and at time 0 this edge read LinkOutData before
    // its assign had settled: a Z symbol on the line under Icarus, and the
    // partner left Detect at once
    TxDataPipe               <= {(DataWidth/8){`NOSIG}};
    TxDataKPipe              <= {(DataWidth/8){1'b1}};
  end
  else
  begin
    // At each cycle, shift right TX PIPE registers, adding link output to top
    TxDataPipe               <= TxDataPipeShift;
    TxDataKPipe              <= TxDataKPipeShift;
  end

end

// openPCIE: the RX side on the falling edge. The core reads LinkIn and
// ElecIdleIn on the rising edge, in VProc's delta cycles; a sample changing on
// that same edge was seen before or after the read as the scheduler chose: one
// symbol apart between simulators, and between XSIM's own scheduling modes
always_ff @(negedge pcieclk_main or negedge nreset)
begin

  if (nreset == 1'b0)
  begin
    rxcount                  <= 0;
    RxDataSample             <= {(DataWidth/8){`NOSIG}};
    RxDataKSample            <= {(DataWidth/8){1'b1}};
  end
  else
  begin
    // Sample the RX input every PIPE width bytes
    if (rxcount == 0)
    begin
      RxDataSample           <= RxData;
      RxDataKSample          <= RxDataK;

      // After sampling, set count to PIPE bytes minus 1
      rxcount                <= (DataWidth/8)-1;
    end
    // When rxcount non-zero, shift right the sample registers and decrement the count
    else
    begin
      RxDataSample           <= RxDataSampleShift;
      RxDataKSample          <= RxDataKSampleShift;

      rxcount                <= rxcount - 1;
    end
  end

end

// At the PIPE clock rate, update TX outputs with TX PIPE data
always_ff @(posedge TxSampleClk or negedge nreset)
begin
  if (nreset == 1'b0)
  begin
    TxData                   <= {(DataWidth/8){`NOSIG}};
    TxDataK                  <= {(DataWidth/8){1'b1}};
  end
  else
  begin
    TxData                   <= TxDataPipe;
    TxDataK                  <= TxDataKPipe;
  end
end

//-------------------------------------------------------------
// PcieVhost configured with x1 link
//-------------------------------------------------------------

  PcieVhost #(
    . LinkWidth         (LINKWIDTH),
    . NodeNum           (NodeNum),
    . EndPoint          (EndPoint),
    . DisableScrambling (0),
    . Disable8b10b      (1),
    . Gen2Clk           (Gen2Clk)
  ) pcievh_i
  (
    .Clk                   (pcieclk),  // Use input pcieclk clock as PcieVhost will divide inside
    .notReset              (nreset),
    .Gen2ClkSel            (Gen2ClkSel),

    .ClkOut                (ClkOut),

    .ElecIdleOut           (ElecIdleOutInt),    // openPCIE: on every simulator
    .ElecIdleIn            ({15'h7fff, RxIdle}),

     // Link lane input
    .LinkIn0               (LinkIn),

    // Unused inputs (openPCIE: 0, idle on ElecIdleIn; upstream Z)
    .LinkIn1               (10'h000), .LinkIn2  (10'h000), .LinkIn3  (10'h000),
    .LinkIn4               (10'h000), .LinkIn5  (10'h000), .LinkIn6  (10'h000),
    .LinkIn7               (10'h000), .LinkIn8  (10'h000), .LinkIn9  (10'h000),
    .LinkIn10              (10'h000), .LinkIn11 (10'h000), .LinkIn12 (10'h000),
    .LinkIn13              (10'h000), .LinkIn14 (10'h000), .LinkIn15 (10'h000),

    // Link lane output
    .LinkOut0              (LinkOut),

    // Unused outputs
    .LinkOut1              (), .LinkOut2  (), .LinkOut3  (),
    .LinkOut4              (), .LinkOut5  (), .LinkOut6  (),
    .LinkOut7              (), .LinkOut8  (), .LinkOut9  (),
    .LinkOut10             (), .LinkOut11 (), .LinkOut12 (),
    .LinkOut13             (), .LinkOut14 (), .LinkOut15 (),

    // Transaction taps
    .TlpTxData             (TlpTxData),   .TlpTxSop (TlpTxSop), .TlpTxEop (TlpTxEop), .TlpTxValid (TlpTxValid),
    .TlpRxData             (TlpRxData),   .TlpRxSop (TlpRxSop), .TlpRxEop (TlpRxEop), .TlpRxValid (TlpRxValid),
    .DllpTxData            (DllpTxData),  .DllpTxValid (DllpTxValid),
    .DllpRxData            (DllpRxData),  .DllpRxValid (DllpRxValid)
  );

endmodule