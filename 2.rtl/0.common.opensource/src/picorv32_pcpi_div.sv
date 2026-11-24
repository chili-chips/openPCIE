// SPDX-FileCopyrightText: 2015 Clifford Wolf
// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: MIT

//--------------------------------------------------------------------------
// Improved by Chili.CHIPS (2023/24)
//--------------------------------------------------------------------------

/***************************************************************
 * picorv32_pcpi_div
 ***************************************************************/

// Chili.CHIPS (2026): control flops (decode, wait, running, sign, quotient
// mask, ready) in their own blocks with an asynchronous reset; data
// (dividend, divisor, quotient, result) without; widths explicit, no 'bx.
// Same cycle behaviour as the original once out of reset.
module picorv32_pcpi_div (
    input  logic        clk,
    input  logic        resetn,

    input  logic        pcpi_valid,
    input  logic [31:0] pcpi_insn,
    input  logic [31:0] pcpi_rs1,
    input  logic [31:0] pcpi_rs2,

    output logic        pcpi_wr,
    output logic [31:0] pcpi_rd,
    output logic        pcpi_wait,
    output logic        pcpi_ready
);
  logic  instr_div, instr_divu, instr_rem, instr_remu;
  logic  instr_any_div_rem;
  assign instr_any_div_rem = |{instr_div, instr_divu, instr_rem, instr_remu};

  logic  pcpi_wait_q;
  logic  start;
  assign start = pcpi_wait & ~pcpi_wait_q;

  // an RV32M instruction (MUL, MULH*, DIV*, REM*): OP opcode, funct7 0000001
  logic        insn_is_rv32m;
  assign       insn_is_rv32m = (pcpi_insn[6:0] == 7'b0110011) & (pcpi_insn[31:25] == 7'b0000001);
  logic        div_insn;  // one of the four divides, not yet answered
  assign       div_insn      = pcpi_valid & ~pcpi_ready & insn_is_rv32m;
  logic [14:0] NC_pcpi_insn;  // the registers: the CPU hands their values over
  assign       NC_pcpi_insn  = {pcpi_insn[24:15], pcpi_insn[11:7]};

  always_ff @(posedge clk or negedge resetn) begin : _decode
    if (!resetn) begin
      instr_div   <= 1'b0;
      instr_divu  <= 1'b0;
      instr_rem   <= 1'b0;
      instr_remu  <= 1'b0;
      pcpi_wait   <= 1'b0;
      pcpi_wait_q <= 1'b0;
    end
    else begin
      instr_div   <= div_insn & (pcpi_insn[14:12] == 3'b100);
      instr_divu  <= div_insn & (pcpi_insn[14:12] == 3'b101);
      instr_rem   <= div_insn & (pcpi_insn[14:12] == 3'b110);
      instr_remu  <= div_insn & (pcpi_insn[14:12] == 3'b111);
      pcpi_wait   <= instr_any_div_rem;
      pcpi_wait_q <= pcpi_wait;
    end
  end : _decode

  logic [31:0] dividend;
  logic [62:0] divisor;
  logic [31:0] quotient;
  logic [31:0] quotient_msk;
  logic        running;
  logic        outsign;
  logic        div_done;  // the last quotient bit is in: answer
  assign       div_done = running & (quotient_msk == '0);
  logic        rs1_neg;  // the operand is negated first (DIV, REM)
  assign       rs1_neg  = (instr_div | instr_rem) & pcpi_rs1[31];
  logic        rs2_neg;
  assign       rs2_neg  = (instr_div | instr_rem) & pcpi_rs2[31];

  always_ff @(posedge clk or negedge resetn) begin : _div_ctrl
    if (!resetn) begin
      running      <= 1'b0;
      outsign      <= 1'b0;
      quotient_msk <= '0;
      pcpi_ready   <= 1'b0;
      pcpi_wr      <= 1'b0;
    end
    else begin
      pcpi_ready <= 1'b0;
      pcpi_wr    <= 1'b0;
      if (start) begin
        running <= 1'b1;
        outsign      <= (instr_div & (pcpi_rs1[31] ^ pcpi_rs2[31]) & (|pcpi_rs2))
                     | (instr_rem & pcpi_rs1[31]);
        quotient_msk <= 32'h8000_0000;
      end
      else if (div_done) begin
        running    <= 1'b0;
        pcpi_ready <= 1'b1;
        pcpi_wr    <= 1'b1;
      end
      else begin
`ifdef RISCV_FORMAL_ALTOPS
        quotient_msk <= quotient_msk >> 5;
`else
        quotient_msk <= quotient_msk >> 1;
`endif
      end
    end
  end : _div_ctrl

  always_ff @(posedge clk) begin : _div_data
    if (start) begin
      dividend <= rs1_neg ? -pcpi_rs1 : pcpi_rs1;
      divisor  <= {rs2_neg ? -pcpi_rs2 : pcpi_rs2, 31'd0};
      quotient <= '0;
    end
    else if (div_done) begin
`ifdef RISCV_FORMAL_ALTOPS
      unique case (1'b1)
        instr_div:  pcpi_rd <= (pcpi_rs1 - pcpi_rs2) ^ 32'h7f8529ec;
        instr_divu: pcpi_rd <= (pcpi_rs1 - pcpi_rs2) ^ 32'h10e8fd70;
        instr_rem:  pcpi_rd <= (pcpi_rs1 - pcpi_rs2) ^ 32'h8da68fa5;
        instr_remu: pcpi_rd <= (pcpi_rs1 - pcpi_rs2) ^ 32'h3138d0e1;
        default: begin
        end
      endcase
`else
      if (instr_div | instr_divu) begin
        pcpi_rd <= outsign ? -quotient : quotient;
      end
      else begin
        pcpi_rd <= outsign ? -dividend : dividend;
      end
`endif
    end
    else begin
      if (divisor <= {31'd0, dividend}) begin
        dividend <= dividend - divisor[31:0];
        quotient <= quotient | quotient_msk;
      end
      divisor <= divisor >> 1;
    end
  end : _div_data

endmodule : picorv32_pcpi_div
