// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// openPCIE * NLnet-sponsored open-source implementation
//--------------------------------------------------------------------------
//                   Copyright (C) 2026 Chili.CHIPS
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//              https://opensource.org/license/bsd-3-clause
//--------------------------------------------------------------------------

module buffer_bank #(
  parameter [3:0] LINK_CAP_MAX_LINK_SPEED = 4'h1,
  parameter [5:0] LINK_CAP_MAX_LINK_WIDTH = 6'h08,
  parameter       IMPL_TARGET       = "HARD",
  parameter       NUM_BRAMS         = 0,
  parameter       RAM_RADDR_LATENCY = 1,
  parameter       RAM_RDATA_LATENCY = 1,
  parameter       RAM_WRITE_LATENCY = 1
) (
  input          user_clk_i,
  mim_if.ram     mim                // packet buffer port of PCIE_2_1 (see mim_if.sv)
);

  localparam DOB_REG = (RAM_RDATA_LATENCY > 1) ? 1 : 0;
  localparam [6:0] WIDTH = (NUM_BRAMS == 1) ? 72 :
                           (NUM_BRAMS == 2) ? 36 :
                           (NUM_BRAMS == 4) ? 18 :
                           (NUM_BRAMS == 8) ?  9 : 4;

  // the PCIE_2_1 data is 68 or 69 bits wide; the tiles are 72
  wire        wen   = mim.wen;
  wire [12:0] waddr = mim.waddr;
  wire [71:0] wdata = mim.wdata;
  wire        ren   = mim.ren;
  wire [12:0] raddr = mim.raddr;
  wire [71:0] rdata;
  assign mim.rdata  = rdata;

  wire        wen_int;
  wire [12:0] waddr_int;
  wire [71:0] wdata_int;
  wire        ren_int;
  wire [12:0] raddr_int;
  wire [71:0] rdata_int;

  generate
    if (RAM_WRITE_LATENCY == 1) begin : wr_lat_2
      logic        wen_q;
      logic [12:0] waddr_q;
      logic [71:0] wdata_q;
      always_ff @(posedge user_clk_i) begin
        wen_q   <= wen;
        waddr_q <= waddr;
        wdata_q <= wdata;
      end
      assign wen_int   = wen_q;
      assign waddr_int = waddr_q;
      assign wdata_int = wdata_q;
    end else begin : wr_lat_1
      assign wen_int   = wen;
      assign waddr_int = waddr;
      assign wdata_int = wdata;
    end

    if (RAM_RADDR_LATENCY == 1) begin : raddr_lat_2
      logic        ren_q;
      logic [12:0] raddr_q;
      always_ff @(posedge user_clk_i) begin
        ren_q   <= ren;
        raddr_q <= raddr;
      end
      assign ren_int   = ren_q;
      assign raddr_int = raddr_q;
    end else begin : raddr_lat_1
      assign ren_int   = ren;
      assign raddr_int = raddr;
    end

    if (RAM_RDATA_LATENCY == 3) begin : rdata_lat_3
      logic [71:0] rdata_q;
      always_ff @(posedge user_clk_i) rdata_q <= rdata_int;
      assign rdata = rdata_q;
    end else begin : rdata_lat_1_2
      assign rdata = rdata_int;
    end
  endgenerate

  // One RAMB36 per tile, WIDTH bits of the 72-bit word each
  localparam ADDR_MSB   = (WIDTH == 4)  ? 12 :
                          (WIDTH == 9)  ? 11 :
                          (WIDTH == 18) ? 10 :
                          (WIDTH == 36) ?  9 : 8;
  localparam WRITE_MODE = ((LINK_CAP_MAX_LINK_SPEED == 4'h2) && (LINK_CAP_MAX_LINK_WIDTH == 6'h08))
                          ? "WRITE_FIRST" : "NO_CHANGE";
  localparam DEVICE     = (IMPL_TARGET == "HARD") ? "7SERIES" : "VIRTEX6";

  generate
    for (genvar ii = 0; ii < NUM_BRAMS; ii = ii + 1) begin : tiles
      BRAM_TDP_MACRO #(
        .DEVICE        (DEVICE),
        .BRAM_SIZE     ("36Kb"),
        .DOA_REG       (0),
        .DOB_REG       (DOB_REG),
        .READ_WIDTH_A  (WIDTH),
        .READ_WIDTH_B  (WIDTH),
        .WRITE_WIDTH_A (WIDTH),
        .WRITE_WIDTH_B (WIDTH),
        .WRITE_MODE_A  (WRITE_MODE)
      ) ramb36 (
        .DOA    (),
        .DOB    (rdata_int[(ii*WIDTH) +: WIDTH]),
        .ADDRA  (waddr_int[ADDR_MSB:0]),
        .ADDRB  (raddr_int[ADDR_MSB:0]),
        .CLKA   (user_clk_i),
        .CLKB   (user_clk_i),
        .DIA    (wdata_int[(ii*WIDTH) +: WIDTH]),
        .DIB    ('0),
        .ENA    (wen_int),
        .ENB    (ren_int),
        .REGCEA (1'b0),
        .REGCEB (1'b1),
        .RSTA   (1'b0),
        .RSTB   (1'b0),
        .WEA    ('1),
        .WEB    ('0)
      );
    end
  endgenerate

endmodule
// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
