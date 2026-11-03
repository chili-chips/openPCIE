//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   Test bench of the Artix-7 RC model in the hard macro's socket (make
//   RC=model PHY=hm): pcie_top of the RTL with models/pcie_core.RC-model.sv
//   in place of pcie_core, so the model meets the endpoint through the PHY
//   and clocks the hard macro has (clk_rst_gen's MMCM and PCLK switch,
//   models/phy_top.EP-PIPE-BFM.sv). No SOC: the user side of pcie_top is
//   idle.
//
//        u_pcie  pcie_top
//          |- clk_rst_gen
//          |- pcie_core  <-- REPLACED --> RC model + pipe_mac_adapter
//          `- phy_top    <-- REPLACED --> pipe_phy_core + the endpoint
//
//   The module is called tb, like tb.sv: the pcieVHost headers call tb.Fatal.
//   The run ends when the model has checked the endpoint's answers (RC MODEL
//   RESULT), or at the time limit of run_us.cfg.
//==========================================================================

`timescale 1ps/1ps

module tb;

  import pcie_pkg::*;

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
// The 100 MHz REFCLK and PERST#, as in tb.sv
//--------------------------------------------------------------

  localparam int HALF_REFCLK_PERIOD_PS = 5_000;   // 100 MHz
  localparam int RST_CYCLES            = 200;

  bit   refclk_p;
  logic perst_n;

  initial begin
    get_run_length();
    perst_n = 1'b0;
    #(HALF_REFCLK_PERIOD_PS * RST_CYCLES * 1ps) perst_n = 1'b1;
    #(RUN_SIM_US * 1us - HALF_REFCLK_PERIOD_PS * RST_CYCLES * 1ps);
    $fatal(1, "time limit of %0d us: no RC MODEL RESULT", RUN_SIM_US);
  end

  always #(HALF_REFCLK_PERIOD_PS * 1ps) refclk_p = ~refclk_p;

//--------------------------------------------------------------
// pcie_top, with the user side idle
//--------------------------------------------------------------

  wire [link_pkg::PCIE_LANES-1:0] rxp, rxn, txp, txn;  // unused: the link runs over PIPE

  assign rxp = '0;
  assign rxn = '1;

  axis_if #(
      .DATA_W(TRN_DATA_W),
      .USER_W($bits(pcie_tx_tuser_t))
  ) s_axis_tx ();
  axis_if #(
      .DATA_W(TRN_DATA_W),
      .USER_W($bits(pcie_rx_tuser_t))
  ) m_axis_rx ();

  cfg_if cfg ();

  assign s_axis_tx.tdata  = '0;
  assign s_axis_tx.tkeep  = '0;
  assign s_axis_tx.tuser  = '0;
  assign s_axis_tx.tlast  = 1'b0;
  assign s_axis_tx.tvalid = 1'b0;
  assign m_axis_rx.tready = 1'b1;

  pcie_top u_pcie (
      .pci_exp_txp(txp),
      .pci_exp_txn(txn),
      .pci_exp_rxp(rxp),
      .pci_exp_rxn(rxn),

      .refclk_p(refclk_p),
      .refclk_n(~refclk_p),
      .perst_n (perst_n),

      .user_clk   (),
      .core_rst_n (),
      .trn_link_up(),

      .s_axis_tx(s_axis_tx),
      .m_axis_rx(m_axis_rx),

      .cfg(cfg)
  );

`ifdef DUMP_VCD
  initial begin
    $dumpfile("tb.vcd");
    $dumpvars(1, tb.u_pcie.u_core.u_rc);
    $dumpvars(1, tb.u_pcie.u_core.u_mac);
    $dumpvars(1, tb.u_pcie.u_phy_top.u_pcie_ep);
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
