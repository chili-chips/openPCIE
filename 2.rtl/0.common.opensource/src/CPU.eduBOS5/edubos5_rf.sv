// SPDX-FileCopyrightText: 2022 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// eduBOS5 register file, reset handled in software
// 2 read ports, asynchronous (LUT RAM) or registered (block RAM, SYNC_READ),
// 1 synchronous write port
//==========================================================================
module edubos5_rf
  import edubos5_pkg::*;
#(
    parameter bit TARGET_IS_ASIC = 1'b0,  // 1: x0 forced to 0 by a read mux, not RAM init
    // 1: registered reads, loaded when rf_re, for an FPGA with no LUT RAM
    // (GateMate): the register file goes to block RAM. 3-stage core only
    parameter bit SYNC_READ      = 1'b0
) (
    input  logic            clk,
    input  logic      [4:0] rs1_addr,
    input  logic      [4:0] rs2_addr,
    input  logic            rf_re,     // SYNC_READ: load rs1, rs2; else not used
    input  logic      [4:0] rd_addr,
    input  logic            rf_we,
    input  cpu_data_t       rf_wdat,

    output cpu_data_t       rs1,
    output cpu_data_t       rs2
);

  // rf[0] is x0 and never written. FPGA: it reads 0 because the RAM starts at
  // 0, its INIT value. ASIC: no power-up value, so a read mux forces it.
  //
  // A plain always, not always_ff, by design (the SOC's RAM rule, as for its
  // ram in riscv_pcie_soc.sv): IEEE 1800-2017 9.2.2.4 forbids any other
  // process to write a variable that an always_ff writes, and the initial
  // block that sets rf to 0 (the RAM's INIT) is such a process. Plain always
  // is the legal form, and the one in the Vivado and Yosys RAM templates, so
  // both still infer a RAM. Standalone slang and VCS reject the always_ff
  // form; read_slang and most simulators let it pass, so do not "fix" this back.
  generate
    if (SYNC_READ) begin : g_sync_read
      cpu_data_t rf[32]  /* synthesis syn_ramstyle = "block_ram" */;

      initial begin
        if (!TARGET_IS_ASIC) begin
          for (int i = 0; i < 32; i++) begin
            rf[i] = '0;
          end
        end
      end

      // The core never writes and reads in the same clock (rf_we in
      // FETCH_INSTR, rf_re in WAIT4REGS), so the RAM's read-during-write
      // mode does not matter
      always @(posedge clk) begin : rf_ports
        if (rf_we == HI) begin
          rf[rd_addr] <= rf_wdat;  // one write port
        end
        if (rf_re == HI) begin
          rs1 <= (TARGET_IS_ASIC & rs1_addr == '0) ? '0 : rf[rs1_addr];
          rs2 <= (TARGET_IS_ASIC & rs2_addr == '0) ? '0 : rf[rs2_addr];
        end
      end : rf_ports
    end
    else begin : g_async_read
      cpu_data_t rf       [32]  /* synthesis syn_ramstyle = "distributed_ram" */;
      logic      NC_rf_re;

      initial begin
        if (!TARGET_IS_ASIC) begin
          for (int i = 0; i < 32; i++) begin
            rf[i] = '0;
          end
        end
      end

      assign NC_rf_re = rf_re;
      assign rs1      = (TARGET_IS_ASIC & rs1_addr == '0) ? '0 : rf[rs1_addr];
      assign rs2      = (TARGET_IS_ASIC & rs2_addr == '0) ? '0 : rf[rs2_addr];

      always @(posedge clk) begin : rf_writeback
        if (rf_we == HI) begin
          rf[rd_addr] <= rf_wdat;  // one write port
        end
      end : rf_writeback
    end
  endgenerate

endmodule : edubos5_rf

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 5, 2024
// -----------------------------------------------------------------------------
