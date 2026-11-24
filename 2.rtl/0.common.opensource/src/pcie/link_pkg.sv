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

// Link width: 4 (default) or 1, set by the build with  PCIE_LANES=1 / =4.
// x4 runs only on the direct link: RC-switched is x1.
`ifndef PCIE_LANES
`define PCIE_LANES 4
`endif

// Link speed: 1 (Gen1) or 2 (Gen2), set by the build with  PCIE_GEN=1 / =2.
// Default (0): Gen1 at x4 (x4 Gen2 closes timing in Vivado only), Gen2 at x1.
// Width and speed set the user clock (pcie_pkg.sv).
`ifndef PCIE_GEN
`define PCIE_GEN 0
`endif

package link_pkg;

  localparam int PCIE_LANES = `PCIE_LANES;

  // Link speed: 1 = Gen1, 2 = Gen2.
  //
  // SIM_GEN1_ONLY pins it to Gen1 for co-simulation (5.sim: make SIM_GEN=1).
  // Only the simulation file list defines it.
`ifdef SIM_GEN1_ONLY
  localparam int PCIE_GEN = 1;
`else
  localparam int PCIE_GEN = (`PCIE_GEN != 0) ? `PCIE_GEN : (PCIE_LANES == 4) ? 1 : 2;
`endif

  localparam int PIPE_MAX_LANES = 8;


  typedef struct packed {
    logic [1:0]  char_is_k;   // [21:20]
    logic [15:0] data;        // [19:4]
    logic        compliance;  // [3]
    logic        elec_idle;   // [2]
    logic [1:0]  powerdown;   // [1:0]
  } pipe_tx_lane_t;

  typedef struct packed {
    logic [1:0]  char_is_k;      // [24:23]
    logic [15:0] data;           // [22:7]
    logic        valid;          // [6]
    logic        chanisaligned;  // [5]
    logic [2:0]  status;         // [4:2]
    logic        phy_status;     // [1]
    logic        elec_idle;      // [0]
  } pipe_rx_lane_t;

  typedef struct packed {
    logic       rcvr_det;  // [6]
    logic       reset;     // [5]
    logic       rate;      // [4]
    logic       deemph;    // [3]
    logic [2:0] margin;    // [2:0]
  } pipe_tx_ctrl_t;

  localparam pipe_rx_lane_t PIPE_RX_LANE_RST = '{
      char_is_k: 2'b00,
      data: 16'h0,
      valid: 1'b0,
      chanisaligned: 1'b0,
      status: 3'b000,
      phy_status: 1'b0,
      elec_idle: 1'b0
  };
  localparam pipe_tx_lane_t PIPE_TX_LANE_RST = '{
      char_is_k: 2'b00,
      data: 16'h0,
      compliance: 1'b0,
      elec_idle: 1'b1,
      powerdown: 2'b10
  };
  localparam pipe_tx_ctrl_t PIPE_TX_CTRL_RST = '{
      rcvr_det: 1'b0,
      reset: 1'b1,
      rate: 1'b0,
      deemph: 1'b1,
      margin: 3'b000
  };

  localparam pipe_rx_lane_t PIPE_RX_LANE_TIE = '{
      char_is_k: 2'b00,
      data: 16'h0,
      valid: 1'b0,
      chanisaligned: 1'b0,
      status: 3'b000,
      phy_status: 1'b0,
      elec_idle: 1'b1
  };
  typedef struct packed {
    logic [15:0] status;     // [111:96]
    logic [15:0] command;    // [95:80]
    logic [15:0] dstatus;    // [79:64]
    logic [15:0] dcommand;   // [63:48]
    logic [15:0] lstatus;    // [47:32]
    logic [15:0] lcommand;   // [31:16]
    logic [15:0] dcommand2;  // [15:0]
  } cfg_regs_t;

  // GTP reset sequencing from init_ctrl, common to all lanes
  typedef struct packed {
    logic gtpll0_reset;    // [8]   PLL / lane logic reset
    logic rxusrclk_reset;  // [7]
    logic drp_clk_reset;   // [6]
    logic gtreset;         // [5]   GTTXRESET, GTRXRESET
    logic drp_start;       // [4]   start the DRP rate retune
    logic drp_x16;         // [3]   ... to the 16-bit internal width
    logic userrdy;         // [2]   TXUSERRDY, RXUSERRDY
    logic txsync_start;    // [1]   start TX phase alignment
    logic idle;            // [0]   sequence done
  } gtp_rst_t;

  // GTPE2_CHANNEL dynamic reconfiguration port (DRP), from chan_retune
  typedef struct packed {
    logic [8:0]  addr;  // [26:18] DRPADDR
    logic        en;    // [17]   DRPEN
    logic [15:0] di;    // [16:1] DRPDI
    logic        we;    // [0]    DRPWE
  } gtp_drp_req_t;

  typedef struct packed {
    logic [15:0] dout;  // [16:1] DRPDO
    logic        rdy;   // [0]    DRPRDY
  } gtp_drp_rsp_t;

  // Gen1 / Gen2 rate change of one lane, from speed_ctrl
  typedef struct packed {
    logic       pipe_clk_sel;  // [8]   PIPE clock: 0 = 125 MHz (Gen1), 1 = 250 MHz (Gen2)
    logic       drp_start;     // [7]   start the DRP retune
    logic       drp_x16;       // [6]   ... to the 16-bit internal width
    logic [2:0] rate;          // [5:3] TXRATE / RXRATE
    logic       txsync_start;  // [2]   start TX phase alignment
    logic       done;          // [1]
    logic       idle;          // [0]
  } gtp_rate_t;

  // GTP TX buffer-bypass phase alignment (UG482): control from phase_align
  // to the GTPE2_CHANNEL, status from the channels (all active lanes)
  typedef struct packed {
    logic dlysreset;  // [3]   TXDLYSRESET
    logic phinit;     // [2]   TXPHINIT
    logic phalign;    // [1]   TXPHALIGN
    logic dlyen;      // [0]   TXDLYEN
  } gtp_txsync_ctrl_t;

  typedef struct packed {
    logic phinitdone;     // [2]   TXPHINITDONE
    logic dlysresetdone;  // [1]   TXDLYSRESETDONE
    logic phaligndone;    // [0]   TXPHALIGNDONE
  } gtp_txsync_stat_t;

endpackage
// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
