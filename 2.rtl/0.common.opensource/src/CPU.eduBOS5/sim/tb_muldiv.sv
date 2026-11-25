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
// Product reuse (mul/mulh fusion): a second block per vector runs multiplies
// that must reuse the kept product (same rs1, rs2, not written since) and
// ones that must not (other kind, rd = rs1, a write or load to rs2), checks
// their results, and that each takes the hit or the miss clock count.
//==========================================================================
module tb_muldiv;
  import edubos5_pkg::*;
  parameter int EN_WAIT4REGS = 0;
  parameter bit STORE_WAIT = 1'b0;
  parameter int N_RAND = 2000;
  parameter bit USE_DSP48E1 = 1'b0;  // 1: needs a DSP48E1 model (run-muldiv.sh)
  parameter bit RF_SYNC_READ = 1'b0;  // 1: needs EN_WAIT4REGS = 1

  localparam int N_CORNER = 16;
  localparam int N_VEC = N_CORNER * N_CORNER + N_RAND;
  localparam int VW = 20;  // words per vector: a, b, 8 results, 10 reuse results
  localparam int DBASE = 32'h0001_0000;  // operand pairs and results, VW words each
  localparam int MEM_WORDS = DBASE / 4 + N_VEC * VW;
  localparam int PROG_WORDS = 64;

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
      .USE_DSP48E1     (USE_DSP48E1),
      .RF_SYNC_READ    (RF_SYNC_READ)
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

  // the clocks between retirements: by funct3 in the first block (main_lo to
  // main_hi), by instruction (pc) in the reuse block
  int clocks = 0, since = 0;
  int main_lo, main_hi;  // word addresses of the first block's M instructions
  int pc_n[PROG_WORDS], pc_min[PROG_WORDS], pc_max[PROG_WORDS];
  logic pc_hit[PROG_WORDS] = '{default: 1'b0};  // the reuse block: 1 where the product must be reused
  logic [2:0] f3_ret;
  int pc_ret;
  assign f3_ret = rvfi_insn[14:12];
  assign pc_ret = int'(rvfi_pc_rdata[31:2]);
  initial begin
    for (int i = 0; i < PROG_WORDS; i++) begin
      pc_n[i]   = 0;
      pc_min[i] = 1 << 30;
      pc_max[i] = 0;
    end
  end
  always_ff @(posedge clk) begin
    clocks <= clocks + 1;
    since  <= rvfi_valid ? 1 : since + 1;
    if (arst_n & rvfi_valid & (rvfi_insn[6:0] == 7'b011_0011) & (rvfi_insn[31:25] == 7'd1)) begin
      if (pc_ret >= main_lo && pc_ret <= main_hi) begin
        lat_n[f3_ret]   <= lat_n[f3_ret] + 1;
        lat_min[f3_ret] <= (since < lat_min[f3_ret]) ? since : lat_min[f3_ret];
        lat_max[f3_ret] <= (since > lat_max[f3_ret]) ? since : lat_max[f3_ret];
      end
      else if (pc_ret < PROG_WORDS) begin
        pc_n[pc_ret]   <= pc_n[pc_ret] + 1;
        pc_min[pc_ret] <= (since < pc_min[pc_ret]) ? since : pc_min[pc_ret];
        pc_max[pc_ret] <= (since > pc_max[pc_ret]) ? since : pc_max[pc_ret];
      end
    end
  end

  initial begin
    int p, errors, v, loop_pc;
    logic [31:0] a, b, m, got, exp, last_end;
    logic [31:0] rexp[10];
    for (int i = 0; i < MEM_WORDS; i++) begin
      mem[i] = '0;
    end
    // operands
    v = 0;
    for (int i = 0; i < N_CORNER; i++) begin
      for (int j = 0; j < N_CORNER; j++) begin
        mem[DBASE/4+VW*v]   = CORNER[i];
        mem[DBASE/4+VW*v+1] = CORNER[j];
        v++;
      end
    end
    for (int i = 0; i < N_RAND; i++) begin
      mem[DBASE/4+VW*v]   = $urandom;
      mem[DBASE/4+VW*v+1] = (i % 4 == 0) ? 32'($urandom % 512) - 256 : $urandom;  // small divisors too
      v++;
    end
    // program: x10 vector pointer, x11 end
    last_end = DBASE + 4 * VW * N_VEC;
    p = 0;
    mem[p++] = lui(10, DBASE);
    mem[p++] = lui(11, last_end + 32'h800);
    mem[p++] = addi(11, 11, 32'(last_end) - 32'({(last_end + 32'h800) >> 12, 12'd0}));
    loop_pc = p;
    mem[p++] = lw(1, 10, 0);
    mem[p++] = lw(2, 10, 4);
    main_lo = p;
    mem[p++] = r_op(0, 3, 1, 2);  // mul, then mulh back to back: the mulh reuses
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
    main_hi = p;
    mem[p++] = r_op(7, 1, 1, 2);  // remu, rd = rs1
    mem[p++] = sw(1, 10, 36);
    // product reuse: pc_hit marks the multiplies that must reuse
    mem[p++] = lw(1, 10, 0);
    mem[p++] = lw(2, 10, 4);
    mem[p++] = r_op(1, 13, 1, 2);  // mulh: miss (x1, x2 loaded)
    mem[p++] = sw(13, 10, 40);
    pc_hit[p] = 1'b1;
    mem[p++] = r_op(0, 14, 1, 2);  // mul: hit
    mem[p++] = sw(14, 10, 44);
    mem[p++] = r_op(3, 15, 1, 2);  // mulhu: miss (other kind)
    mem[p++] = sw(15, 10, 48);
    mem[p++] = r_op(2, 16, 1, 2);  // mulhsu: miss (other kind)
    mem[p++] = sw(16, 10, 52);
    pc_hit[p] = 1'b1;
    mem[p++] = r_op(0, 1, 1, 2);  // mul x1 = lo(a*b): hit (a low word from any kind)
    mem[p++] = sw(1, 10, 56);
    mem[p++] = r_op(1, 17, 1, 2);  // mulh: miss (x1 written)
    mem[p++] = sw(17, 10, 60);
    pc_hit[p] = 1'b1;
    mem[p++] = r_op(0, 18, 1, 2);  // mul: hit
    mem[p++] = sw(18, 10, 64);
    mem[p++] = addi(2, 2, 1);
    mem[p++] = r_op(1, 19, 1, 2);  // mulh: miss (x2 written)
    mem[p++] = sw(19, 10, 68);
    mem[p++] = lw(2, 10, 4);
    mem[p++] = r_op(1, 20, 1, 2);  // mulh: miss (x2 loaded)
    mem[p++] = sw(20, 10, 72);
    pc_hit[p] = 1'b1;
    mem[p++] = r_op(0, 0, 1, 2);  // mul x0: hit, writes nothing
    pc_hit[p] = 1'b1;
    mem[p++] = r_op(1, 21, 1, 2);  // mulh: hit
    mem[p++] = sw(21, 10, 76);
    mem[p++] = addi(10, 10, 4 * VW);
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
      a = mem[DBASE/4+VW*v];
      b = mem[DBASE/4+VW*v+1];
      for (int f3 = 0; f3 < 8; f3++) begin
        got = mem[DBASE/4+VW*v+RES_WORD[f3]];
        exp = ref_md(f3, a, b);
        if (got !== exp) begin
          if (errors < 20) begin
            $display("FAIL: funct3 %0d a %h b %h: got %h, expected %h", f3, a, b, got, exp);
          end
          errors++;
        end
      end
      // the reuse block, in program order (m = lo(a*b), x1 from then on)
      m       = ref_md(0, a, b);
      rexp[0] = ref_md(1, a, b);
      rexp[1] = m;
      rexp[2] = ref_md(3, a, b);
      rexp[3] = ref_md(2, a, b);
      rexp[4] = m;
      rexp[5] = ref_md(1, m, b);
      rexp[6] = ref_md(0, m, b);
      rexp[7] = ref_md(1, m, b + 32'd1);
      rexp[8] = ref_md(1, m, b);
      rexp[9] = ref_md(1, m, b);
      for (int k = 0; k < 10; k++) begin
        got = mem[DBASE/4+VW*v+10+k];
        if (got !== rexp[k]) begin
          if (errors < 20) begin
            $display("FAIL: reuse case %0d a %h b %h: got %h, expected %h", k, a, b, got, rexp[k]);
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
    // reuse block: a hit takes the clocks of the first block's mulh (a hit),
    // a miss those of its mul (a miss)
    $display("reuse: hit %0d clocks, miss %0d clocks", lat_min[1], lat_min[0]);
    if (!(lat_min[1] < lat_min[0])) begin
      $display("FAIL: the first block's mulh did not reuse the mul's product");
      errors++;
    end
    for (int i = 0; i < PROG_WORDS; i++) begin
      if (pc_n[i] != 0) begin
        exp = pc_hit[i] ? lat_min[1] : lat_min[0];
        if (pc_n[i] != N_VEC || (!STORE_WAIT && (pc_min[i] != exp || pc_max[i] != exp))) begin
          $display("FAIL: reuse pc %h (%s): %0d retired, %0d to %0d clocks, expected %0d", i * 4,
                   pc_hit[i] ? "hit" : "miss", pc_n[i], pc_min[i], pc_max[i], exp);
          errors++;
        end
      end
    end
    $display("%s: %0d vectors x 18 instructions, %0d errors, %0d clocks", errors ? "FAIL" : "PASS",
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
