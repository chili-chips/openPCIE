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

module phy_top
  import link_pkg::*;
(
  input  wire [5:0]                             pl_ltssm_state,

  pipe_if.phy                              pipe_mac,

  output wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     pci_exp_txn,
  output wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     pci_exp_txp,
  input  wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     pci_exp_rxn,
  input  wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     pci_exp_rxp,

  input  wire                                   sys_clk,
  input  wire                                   sys_rst_n,
  input  wire                                   PIPE_MMCM_RST_N,
  output wire                                   pipe_clk,
  output wire                                   user_clk,
  output wire                                   user_clk2,

  input  wire                                   PIPE_PCLK_IN,
  input  wire                                   PIPE_RXUSRCLK_IN,
  input  wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     PIPE_RXOUTCLK_IN,
  input  wire                                   PIPE_DCLK_IN,
  input  wire                                   PIPE_USERCLK1_IN,
  input  wire                                   PIPE_USERCLK2_IN,
  input  wire                                   PIPE_OOBCLK_IN,
  input  wire                                   PIPE_MMCM_LOCK_IN,
  output wire                                   PIPE_TXOUTCLK_OUT,
  output wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     PIPE_RXOUTCLK_OUT,
  output wire [LINK_CAP_MAX_LINK_WIDTH-1:0]     PIPE_PCLK_SEL_OUT,
  output wire                                   PIPE_GEN3_OUT,

  output wire                                   phy_rdy_n
);

  wire [LINK_CAP_MAX_LINK_WIDTH-1:0] phystatus_rst;
  wire                               clock_locked;

  // PIPE towards gtp_wrapper (GTP side, 32-bit per lane)
  pipe_if #(.LANES(LINK_CAP_MAX_LINK_WIDTH), .DATA_W(32)) pipe_phy ();

  wire                               pipe_clk_int;
  logic                                phy_rdy_n_int;
  logic                                reg_clock_locked;
  wire                               all_phystatus_rst;

  assign pipe_clk = pipe_clk_int;

  mac2phy u_mac2phy (
    .pipe_mac       (pipe_mac),
    .pipe_phy       (pipe_phy),
    .pl_ltssm_state (pl_ltssm_state),
    .pipe_clk       (pipe_clk_int),
    .clock_locked   (clock_locked),
    .phy_rdy_n      (phy_rdy_n_int)
  );

  gtp_wrapper u_gtp_wrapper (
    .PIPE_CLK           (sys_clk),
    .PIPE_RESET_N       (sys_rst_n),
    .PIPE_PCLK          (pipe_clk_int),

    .pipe_phy           (pipe_phy),
    .PIPE_TXP           (pci_exp_txp),
    .PIPE_TXN           (pci_exp_txn),

    .PIPE_RXP           (pci_exp_rxp),
    .PIPE_RXN           (pci_exp_rxn),

    .PIPE_PHYSTATUS_RST (phystatus_rst),

    .PIPE_MMCM_RST_N    (PIPE_MMCM_RST_N),
    .PIPE_PCLK_LOCK     (clock_locked),
    .PIPE_USERCLK1      (user_clk),
    .PIPE_USERCLK2      (user_clk2),

    .PIPE_PCLK_IN       (PIPE_PCLK_IN),
    .PIPE_RXUSRCLK_IN   (PIPE_RXUSRCLK_IN),
    .PIPE_RXOUTCLK_IN   (PIPE_RXOUTCLK_IN),
    .PIPE_DCLK_IN       (PIPE_DCLK_IN),
    .PIPE_USERCLK1_IN   (PIPE_USERCLK1_IN),
    .PIPE_USERCLK2_IN   (PIPE_USERCLK2_IN),
    .PIPE_OOBCLK_IN     (PIPE_OOBCLK_IN),
    .PIPE_MMCM_LOCK_IN  (PIPE_MMCM_LOCK_IN),
    .PIPE_TXOUTCLK_OUT  (PIPE_TXOUTCLK_OUT),
    .PIPE_RXOUTCLK_OUT  (PIPE_RXOUTCLK_OUT),
    .PIPE_PCLK_SEL_OUT  (PIPE_PCLK_SEL_OUT),
    .PIPE_GEN3_OUT      (PIPE_GEN3_OUT)
  );

  always_ff @(posedge pipe_clk_int or negedge clock_locked) begin
    if (!clock_locked) reg_clock_locked <= 1'b0;
    else               reg_clock_locked <= 1'b1;
  end

  assign all_phystatus_rst = &phystatus_rst;

  always_ff @(posedge pipe_clk_int) begin
    if (!reg_clock_locked) phy_rdy_n_int <= 1'b0;
    else                   phy_rdy_n_int <= all_phystatus_rst;
  end

  assign phy_rdy_n = phy_rdy_n_int;

endmodule
// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
