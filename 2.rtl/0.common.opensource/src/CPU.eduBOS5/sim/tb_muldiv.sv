// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// RV32M test of eduBOS5 with USE_HW_MUL=1 (run-muldiv.sh). A program loops
// over operand pairs in memory, runs all 8 M instructions on each and stores
// the results; the bench compares them with its own reference. The pairs:
// every combination of corner values (0, 1, -1, INT_MIN, INT_MAX, ...), then
// N_RAND random ones. It also checks that each M instruction takes a fixed
// count of clocks, whatever its operands (RVFI: clocks between retirements).
// STORE_WAIT=1: STORE_WAITS_RDY=1 on a bus that holds stores at random.
//==========================================================================
module tb_muldiv;
  import edubos5_pkg::*;
  parameter int EN_WAIT4REGS = 0;
  parameter bit STORE_WAIT = 1'b0;
  parameter int N_RAND = 2000;
  parameter bit USE_DSP48E1 = 1'b0;  // 1: needs a DSP48E1 model (run-muldiv.sh)

  localparam int N_CORNER = 16;
  localparam int N_VEC = N_CORNER * N_CORNER + N_RAND;
  localparam int DBASE = 32'h0001_0000;  // operand pairs and results, 40 bytes each
  localparam int MEM_WORDS = DBASE / 4 + N_VEC * 10;

  logic clk = 1'b0;
  logic arst_n = 1'b0;
  always begin
    #5ns clk = ~clk;
  end

  logic rvfi_valid, rvfi_trap, rvfi_halt, rvfi_intr;
  logic [63:0] rvfi_order;
  logic [31:0] rvfi_insn, rvfi_rs1_rdata, rvfi_rs2_rdata, rvfi_rd_wdata;
  logic [31:0] rvfi_pc_rdata, rvfi_pc_wdata, rvfi_mem_addr, rvfi_mem_rdata, rvfi_mem_wdata;
  logic [4:0] rvfi_rs1_addr, rvfi_rs2_addr, rvfi_rd_addr;
  logic [3:0] rvfi_mem_rmask, rvfi_mem_wmask;
  logic [1:0] rvfi_mode, rvfi_ixl;

  cpu_pc_t imem_addr;
  cpu_data_t imem_rdat;
  cpu_addr_t dmem_addr;
  we_bs_t dmem_we;
  cpu_data_t dmem_wdat, dmem_rdat;
  logic imem_vld, dmem_strobe, dmem_vld, dmem_rdy;

  logic [31:0] mem[MEM_WORDS];

  // IMEM: synchronous read. DMEM: ready the clock after a request, as the SOC
  // RAM; dmem_rdy only answers a pending read (riscv_pcie_soc.sv, load_wait)
  logic ram_ready, load_wait, store_ok;
  always_ff @(posedge clk) begin
    imem_rdat <= mem[imem_addr];
    dmem_rdat <= mem[dmem_addr[31:2]];
    ram_ready <= dmem_vld;
    load_wait <= dmem_vld & (dmem_we == NOWR) & ~ram_ready;
    store_ok  <= STORE_WAIT ? 1'($urandom % 2) : 1'b1;
    if (dmem_vld & (dmem_we != NOWR) & (store_ok | ~STORE_WAIT)) begin
      for (int i = 0; i < 4; i++) begin
        if (dmem_we[i]) begin
          mem[dmem_addr[31:2]][8*i+:8] <= dmem_wdat[8*i+:8];
        end
      end
    end
  end
  assign dmem_rdy = (ram_ready & load_wait) | (STORE_WAIT & dmem_vld & (dmem_we != NOWR) & store_ok);

  edubos5 #(
      .EN_WAIT4REGS    (EN_WAIT4REGS),
      .STORE_WAITS_RDY (STORE_WAIT),
      .USE_HW_MUL      (1'b1),
      .USE_DSP48E1     (USE_DSP48E1)
  ) uut (
      .clk,
      .arst_n,
      .rvfi_valid,
      .rvfi_order,
      .rvfi_insn,
      .rvfi_trap,
      .rvfi_halt,
      .rvfi_intr,
      .rvfi_mode,
      .rvfi_ixl,
      .rvfi_rs1_addr,
      .rvfi_rs2_addr,
      .rvfi_rs1_rdata,
      .rvfi_rs2_rdata,
      .rvfi_rd_addr,
      .rvfi_rd_wdata,
      .rvfi_pc_rdata,
      .rvfi_pc_wdata,
      .rvfi_mem_addr,
      .rvfi_mem_rmask,
      .rvfi_mem_wmask,
      .rvfi_mem_rdata,
      .rvfi_mem_wdata,
      .imem_addr,
      .imem_rdat,
      .imem_rdy    (1'b1),
      .imem_vld,
      .dmem_addr,
      .dmem_we,
      .dmem_strobe,
      .dmem_wdat,
      .dmem_rdat,
      .dmem_rdy,
      .dmem_vld
  );

  //--- instruction encodings ---
  function automatic logic [31:0] r_op(int f3, int rd, int rs1, int rs2);  // M: funct7 1
    return {7'b000_0001, 5'(rs2), 5'(rs1), 3'(f3), 5'(rd), 7'b011_0011};
  endfunction
  function automatic logic [31:0] lw(int rd, int rs1, int imm);
    return {12'(imm), 5'(rs1), 3'b010, 5'(rd), 7'b000_0011};
  endfunction
  function automatic logic [31:0] sw(int rs2, int rs1, int imm);
    logic [11:0] i = 12'(imm);
    return {i[11:5], 5'(rs2), 5'(rs1), 3'b010, i[4:0], 7'b010_0011};
  endfunction
  function automatic logic [31:0] addi(int rd, int rs1, int imm);
    return {12'(imm), 5'(rs1), 3'b000, 5'(rd), 7'b001_0011};
  endfunction
  function automatic logic [31:0] lui(int rd, logic [31:0] val);
    return {val[31:12], 5'(rd), 7'b011_0111};
  endfunction
  function automatic logic [31:0] bne(int rs1, int rs2, int off);
    logic [12:0] o = 13'(off);
    return {o[12], o[10:5], 5'(rs2), 5'(rs1), 3'b001, o[4:1], o[11], 7'b110_0011};
  endfunction

  //--- reference ---
  function automatic logic [31:0] ref_md(int f3, logic [31:0] a, logic [31:0] b);
    logic signed [63:0] ps;
    logic [63:0] pu;
    unique case (f3)
      0: begin
        pu = {32'd0, a} * {32'd0, b};
        return pu[31:0];
      end
      1: begin
        ps = 64'($signed(a)) * 64'($signed(b));
        return ps[63:32];
      end
      2: begin
        ps = 64'($signed(a)) * $signed({32'd0, b});
        return ps[63:32];
      end
      3: begin
        pu = {32'd0, a} * {32'd0, b};
        return pu[63:32];
      end
      4: begin
        if (b == 0) return 32'hFFFF_FFFF;
        if (a == 32'h8000_0000 && b == 32'hFFFF_FFFF) return 32'h8000_0000;
        return 32'($signed(a) / $signed(b));
      end
      5: return (b == 0) ? 32'hFFFF_FFFF : a / b;
      6: begin
        if (b == 0) return a;
        if (a == 32'h8000_0000 && b == 32'hFFFF_FFFF) return 32'd0;
        return 32'($signed(a) % $signed(b));
      end
      default: return (b == 0) ? a : a % b;
    endcase
  endfunction

  localparam logic [31:0] CORNER[N_CORNER] = '{
      32'h0000_0000, 32'h0000_0001, 32'hFFFF_FFFF, 32'h0000_0002,
      32'hFFFF_FFFE, 32'h8000_0000, 32'h7FFF_FFFF, 32'h8000_0001,
      32'h0001_FFFF, 32'h0002_0000, 32'hFFFF_0000, 32'h8000_FFFF,
      32'h1234_5678, 32'hDEAD_BEEF, 32'h0000_0007, 32'hFFFF_FFF9
  };

  // result offsets in a vector (words 2..9) of each funct3, as the program stores them
  localparam int RES_WORD[8] = '{2, 3, 4, 5, 6, 7, 8, 9};
  // clocks of the retired M instructions, by funct3: count, fewest, most
  int lat_n[8], lat_min[8], lat_max[8];
  initial begin
    for (int i = 0; i < 8; i++) begin
      lat_n[i]   = 0;
      lat_min[i] = 1 << 30;
      lat_max[i] = 0;
    end
  end

  // the clocks between retirements
  int clocks = 0, since = 0;
  logic [2:0] f3_ret;
  assign f3_ret = rvfi_insn[14:12];
  always_ff @(posedge clk) begin
    clocks <= clocks + 1;
    since  <= rvfi_valid ? 1 : since + 1;
    if (arst_n & rvfi_valid & (rvfi_insn[6:0] == 7'b011_0011) & (rvfi_insn[31:25] == 7'd1)) begin
      lat_n[f3_ret]   <= lat_n[f3_ret] + 1;
      lat_min[f3_ret] <= (since < lat_min[f3_ret]) ? since : lat_min[f3_ret];
      lat_max[f3_ret] <= (since > lat_max[f3_ret]) ? since : lat_max[f3_ret];
    end
  end

  initial begin
    int p, errors, v, loop_pc;
    logic [31:0] a, b, got, exp, last_end;
    for (int i = 0; i < MEM_WORDS; i++) begin
      mem[i] = '0;
    end
    // operands
    v = 0;
    for (int i = 0; i < N_CORNER; i++) begin
      for (int j = 0; j < N_CORNER; j++) begin
        mem[DBASE/4+10*v]   = CORNER[i];
        mem[DBASE/4+10*v+1] = CORNER[j];
        v++;
      end
    end
    for (int i = 0; i < N_RAND; i++) begin
      mem[DBASE/4+10*v]   = $urandom;
      mem[DBASE/4+10*v+1] = (i % 4 == 0) ? 32'($urandom % 512) - 256 : $urandom;  // small divisors too
      v++;
    end
    // program: x10 vector pointer, x11 end
    last_end = DBASE + 40 * N_VEC;
    p = 0;
    mem[p++] = lui(10, DBASE);
    mem[p++] = lui(11, last_end + 32'h800);
    mem[p++] = addi(11, 11, 32'(last_end) - 32'({(last_end + 32'h800) >> 12, 12'd0}));
    loop_pc = p;
    mem[p++] = lw(1, 10, 0);
    mem[p++] = lw(2, 10, 4);
    mem[p++] = r_op(0, 3, 1, 2);  // mul, then mulh back to back
    mem[p++] = r_op(1, 4, 1, 2);
    mem[p++] = sw(3, 10, 8);
    mem[p++] = sw(4, 10, 12);
    mem[p++] = r_op(2, 5, 1, 2);  // mulhsu
    mem[p++] = sw(5, 10, 16);
    mem[p++] = r_op(3, 6, 1, 2);  // mulhu
    mem[p++] = sw(6, 10, 20);
    mem[p++] = r_op(4, 7, 1, 2);  // div, then divu back to back
    mem[p++] = r_op(5, 8, 1, 2);
    mem[p++] = sw(7, 10, 24);
    mem[p++] = sw(8, 10, 28);
    mem[p++] = r_op(6, 9, 1, 2);  // rem
    mem[p++] = sw(9, 10, 32);
    mem[p++] = r_op(7, 1, 1, 2);  // remu, rd = rs1
    mem[p++] = sw(1, 10, 36);
    mem[p++] = addi(10, 10, 40);
    mem[p] = bne(10, 11, (loop_pc - p) * 4);
    p++;
    mem[p++] = 32'h0000_0073;  // ecall: eduBOS5 halts

    #25ns arst_n = 1'b1;
    // halted: the fetch address stands still
    begin
      int still = 0;
      cpu_pc_t last = '0;
      while (still < 100 && clocks < 2_000_000_000) begin
        @(posedge clk);
        still = (imem_addr == last) ? still + 1 : 0;
        last  = imem_addr;
      end
      if (imem_addr != cpu_pc_t'(p - 1)) begin
        $display("FAIL: halted at pc %h, not at the ecall", {imem_addr, 2'b00});
        $finish;
      end
    end

    errors = 0;
    for (v = 0; v < N_VEC; v++) begin
      a = mem[DBASE/4+10*v];
      b = mem[DBASE/4+10*v+1];
      for (int f3 = 0; f3 < 8; f3++) begin
        got = mem[DBASE/4+10*v+RES_WORD[f3]];
        exp = ref_md(f3, a, b);
        if (got !== exp) begin
          if (errors < 20) begin
            $display("FAIL: funct3 %0d a %h b %h: got %h, expected %h", f3, a, b, got, exp);
          end
          errors++;
        end
      end
    end
    for (int f3 = 0; f3 < 8; f3++) begin
      $display("funct3 %0d: %0d retired, %0d to %0d clocks", f3, lat_n[f3], lat_min[f3],
               lat_max[f3]);
      if (lat_n[f3] != N_VEC || (!STORE_WAIT && lat_min[f3] != lat_max[f3])) begin
        $display("FAIL: funct3 %0d: count or clocks not as expected", f3);
        errors++;
      end
    end
    $display("%s: %0d vectors x 8 instructions, %0d errors, %0d clocks", errors ? "FAIL" : "PASS",
             N_VEC, errors, clocks);
    $finish;
  end
endmodule : tb_muldiv

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 23, 2026
// -----------------------------------------------------------------------------
