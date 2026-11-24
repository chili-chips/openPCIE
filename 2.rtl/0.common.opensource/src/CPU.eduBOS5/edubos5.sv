// SPDX-FileCopyrightText: 2022 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// LOAD and STORE instructions
//==========================================================================

module edubos5
  import edubos5_pkg::*;
#(
    parameter int EN_WAIT4REGS    = 0,     // 0 for 2-stage CPU pipeline
    parameter bit GOWIN_FAST_OP   = 1'b0,
    parameter bit TARGET_IS_ASIC  = 1'b0,  // 1: x0 forced to 0 by a read mux, not RAM init
    // 1: a STORE holds its request until dmem_rdy, stalling the core; for a
    // bus that can make a store wait. 0: a STORE is a one-cycle request
    parameter bit STORE_WAITS_RDY = 1'b0,
    // 1: the Zicntr counters, read with csrrs/csrrc rd, csr, x0 (rdcycle,
    // rdtime, rdinstret and the h halves; time is cycle, at the fixed clock).
    // Any other SYSTEM instruction halts the core, as with 0
    parameter bit EN_COUNTERS     = 1'b0,
    // 1: RV32M (mul, mulh, mulhsu, mulhu, div, divu, rem, remu), each in a
    // fixed count of clocks whatever its operands; firmware -march=rv32im.
    // 0: RV32I only, no logic
    parameter bit USE_HW_MUL      = 1'b0,
    // USE_HW_MUL: 1 multiplies in a DSP48E1 primitive (Xilinx 7-series), 0 in
    // plain RTL with the same registers and timing
    parameter bit USE_DSP48E1     = 1'b0
) (
    input  logic             clk,
    input  logic             arst_n,          // reset button

`ifdef RISCV_FORMAL
    // RVFI (riscv-formal): one retired instruction per rvfi_valid
    output logic             rvfi_valid,
    output logic      [63:0] rvfi_order,
    output logic      [31:0] rvfi_insn,
    output logic             rvfi_trap,
    output logic             rvfi_halt,
    output logic             rvfi_intr,
    output logic      [ 1:0] rvfi_mode,
    output logic      [ 1:0] rvfi_ixl,
    output logic      [ 4:0] rvfi_rs1_addr,
    output logic      [ 4:0] rvfi_rs2_addr,
    output logic      [31:0] rvfi_rs1_rdata,
    output logic      [31:0] rvfi_rs2_rdata,
    output logic      [ 4:0] rvfi_rd_addr,
    output logic      [31:0] rvfi_rd_wdata,
    output logic      [31:0] rvfi_pc_rdata,
    output logic      [31:0] rvfi_pc_wdata,
    output logic      [31:0] rvfi_mem_addr,
    output logic      [ 3:0] rvfi_mem_rmask,
    output logic      [ 3:0] rvfi_mem_wmask,
    output logic      [31:0] rvfi_mem_rdata,
    output logic      [31:0] rvfi_mem_wdata,
`endif

    //Intruction Memory access
    output cpu_pc_t          imem_addr,
    input  cpu_data_t        imem_rdat,
    input  logic             imem_rdy,        // IMEM answers in one cycle: not used
    output logic             imem_vld,


    //Data Memory access
    output cpu_addr_t        dmem_addr,
    output we_bs_t           dmem_we,
    output logic             dmem_strobe,
    output cpu_data_t        dmem_wdat,
    input  cpu_data_t        dmem_rdat,

    input  logic             dmem_rdy,
    output logic             dmem_vld

);

  cpu_data_t rs1, rf_wdat;
  cpu_data_t rs2;
  logic      rf_we;  // write enable for the register file

  cpu_pc_t   pc;
  cpu_pc_t inc_pc, next_pc, pc_imm, pc_auipc;
  logic pc_jump;  // next_pc is not inc_pc: JAL, JALR, a taken branch

  // STOREs wait on dmem_rdy only with STORE_WAITS_RDY (store_stall below)
  typedef enum logic [2:0] {
    FETCH_INSTR  = 3'd0,
    WAIT4REGS    = 3'd1,
    LOAD_DATA    = 3'd2,
    WAIT_DATA    = 3'd3,
    EXECUTE      = 3'd4,
    FETCH_TARGET = 3'd5   // 3-stage: a jump or taken branch fetches its target
  } state_t;

  state_t       state;

  logic         go2exec;  // pipeline register load enable

  // A STORE is on the bus in the cycle after its FETCH_INSTR, the next
  // instruction's EXECUTE (2-stage) or WAIT4REGS (3-stage). Without ready, the
  // request and that state are held.
  logic         store_stall;

  // ALU input and outputs
  logic   [4:0] shamt;  // SHift AMounT from immediate or rs2
  cpu_data_t aluin_1, aluin_2, alu_out;  // 32-bit IO from the ALU

  logic eq, lt, ltu;

  // RV32M (USE_HW_MUL): an M instruction stays in EXECUTE while md_hold
  logic                md_hold;
  cpu_data_t           alu_md_out;  // to the register file: alu_out, or the M result

  cpu_data_t           dmem_rdat_reg;
  logic                dmem_rdy_reg;

  // no-connects: IMEM ready, the low bits of the word-aligned immediates and
  // the decode bits nothing reads after the decode cycle
  logic                NC_imem_rdy;
  logic          [5:0] NC_imm_low;
  logic          [1:0] NC_decode_reg;

  //-------------------------------------------------
  // instruction decoder
  //-------------------------------------------------
  instr_t              instr;
  instr_decode_t       instr_decode;
  logic                cnt_rd;  // a SYSTEM instruction that reads a counter

  always_comb begin : _instr_decode

    instr_decode = '0;

    unique case (instr.opcode)
      ALUREG:  instr_decode.ALUREG = HI;
      ALUIMM:  instr_decode.ALUIMM = HI;
      BRNCH:   instr_decode.BRNCH = HI;
      JALR:    instr_decode.JALR = HI;
      JAL:     instr_decode.JAL = HI;
      AUIPC:   instr_decode.AUIPC = HI;
      LUI:     instr_decode.LUI = HI;
      LOAD:    instr_decode.LOAD = HI;
      STORE:   instr_decode.STORE = HI;
      FENCE:   instr_decode.FENCE = HI;
      SYSTEM: begin
        instr_decode.SYSTEM = ~cnt_rd;  // a halt
        instr_decode.CNT    = cnt_rd;
      end
      default: instr_decode.SYSTEM = HI;
    endcase
  end : _instr_decode

  instr_decode_t instr_decode_reg;
  assign NC_imem_rdy   = imem_rdy;
  assign NC_decode_reg = {instr_decode_reg.FENCE, instr_decode_reg.SYSTEM};

  //-------------------------------------------------
  // Zicntr counters (EN_COUNTERS)
  //-------------------------------------------------
  cpu_data_t cnt_rdat;  // the counter the instruction reads

  generate
    if (EN_COUNTERS == 1'b1) begin : g_cnt
      // 64-bit cycle and instret as 32-bit halves: the high half takes the
      // carry flagged a clock ahead (*_max: the low half is all ones), so no
      // 64-bit carry chain, and the halves are never out of step
      logic [31:0] cycle_lo, cycle_hi, instret_lo, instret_hi;
      logic cycle_max, instret_max;

      // csrrs/csrrc with rs1 x0, or csrrsi/csrrci with uimm 0 (no write; the
      // same bits), of 0xC00-0xC02 or 0xC80-0xC82
      assign cnt_rd = instr.grp.imm.func3[1]
                    & (instr.grp.reg2reg.rs1 == '0)
                    & (instr.grp.imm.imm11_0[11:8] == 4'hC)
                    & (instr.grp.imm.imm11_0[6:2] == '0)
                    & (instr.grp.imm.imm11_0[1:0] != 2'b11);

      // [1]: instret, else cycle or time; [7]: the high half
      assign cnt_rdat = instr.grp.imm.imm11_0[1]
                      ? (instr.grp.imm.imm11_0[7] ? instret_hi : instret_lo)
                      : (instr.grp.imm.imm11_0[7] ? cycle_hi : cycle_lo);

      always_ff @(negedge arst_n or posedge clk) begin
        if (arst_n == LO) begin
          cycle_lo    <= '0;
          cycle_hi    <= '0;
          cycle_max   <= 1'b0;
          instret_lo  <= '0;
          instret_hi  <= '0;
          instret_max <= 1'b0;
        end
        else begin
          cycle_lo  <= cycle_lo + 32'd1;
          cycle_max <= (cycle_lo == 32'hFFFF_FFFE);
          if (cycle_max) begin
            cycle_hi <= cycle_hi + 32'd1;
          end
          if (state == FETCH_INSTR) begin  // an instruction retires
            instret_lo  <= instret_lo + 32'd1;
            instret_max <= (instret_lo == 32'hFFFF_FFFE);
            if (instret_max) begin
              instret_hi <= instret_hi + 32'd1;
            end
          end
        end
      end
    end
    else begin : g_no_cnt
      assign cnt_rd   = 1'b0;
      assign cnt_rdat = '0;
    end
  endgenerate
  always_ff @(negedge arst_n or posedge clk) begin : _instr_decode_flop
    if (arst_n == LO) begin
      instr_decode_reg <= '0;
    end
    else if (go2exec) begin
      instr_decode_reg <= instr_decode;
    end
  end : _instr_decode_flop
  //-------------------------------------------------
  // assembly of immediates
  //-------------------------------------------------
  cpu_data_t imm_iload, imm_uload, imm_store, imm_brnch, imm_jump;

  always_comb begin : _immediates
    imm_iload = {
      {20{instr.grp.imm.imm11_0[11]}},  //[31:12] upper part is sign-expanded
      instr.grp.imm.imm11_0
    };  //[11:0]  LOWER part is from instruction

    imm_uload = {
      instr.grp.uimm.imm31_12,  //[31:12] UPPER part is from instruction
      12'd0
    };  //[11:0]  lower part is dummy

    imm_store = {
      {20{instr.grp.store.imm11_5[11]}},  //[31:12] upper part is sign-expanded
      instr.grp.store.imm11_5,  //-\ [11:0]
      instr.grp.store.imm4_0
    };  //-/

    imm_brnch = {
      {20{instr.grp.brnch.imm12}},  //[31:12] sign-expanded
      instr.grp.brnch.imm11,  //-\ [11:1]
      instr.grp.brnch.imm10_5,  // |
      instr.grp.brnch.imm4_1,  //-/
      1'b0
    };  //[0]

    imm_jump = {
      {12{instr.grp.jump.imm20}},  //[31:20] sign-expanded
      instr.grp.jump.imm19_12,  //-\ [19:1]
      instr.grp.jump.imm11,  // |
      instr.grp.jump.imm10_1,  //-/
      1'b0
    };  //[0]
  end : _immediates

  // flopping the immediates, solving timing by being inputs to combo strucutres
  funct3_brnch_t funct3_brnch;
  funct3_load_t  funct3_imm;
  logic          funct7_alu;

  cpu_data_t     imm_uload_reg;
  cpu_pc_t       pc_imm_reg;

  assign NC_imm_low = {imm_brnch[1:0], imm_jump[1:0], imm_uload_reg[1:0]};

  // the functs select: reset; the immediates are data
  always_ff @(negedge arst_n or posedge clk) begin
    if (arst_n == LO) begin
      funct3_brnch <= funct3_brnch_t'(0);
      funct7_alu   <= 1'b0;
      funct3_imm   <= funct3_load_t'(0);
    end
    else if (go2exec) begin
      funct3_brnch <= instr.grp.brnch.func3;
      funct7_alu   <= instr.grp.reg2reg.func7[5];
      funct3_imm   <= instr.grp.imm.func3;
    end
  end

  always_ff @(posedge clk) begin
    if (go2exec) begin
      pc_imm_reg    <= pc_imm;
      imm_uload_reg <= imm_uload;
    end
  end

  //-----------------------------------------------------------------
  // 2 RF read ports for reg fetch, address out, data and instr fetch
  //-----------------------------------------------------------------
  //drive instr address
  // 3-stage: FETCH_INSTR fetches pc + 1, from pc alone. next_pc, from the ALU,
  // goes only to pc; after a jump FETCH_TARGET fetches from it. So the RAM
  // address never waits for the ALU (1 more cycle per jump or taken branch).
  always_comb begin : _fetch_regs_instr
    if (state == FETCH_INSTR) begin
      imem_addr = (EN_WAIT4REGS == 1) ? inc_pc : next_pc[31:2];
    end
    else begin
      imem_addr = pc;
    end
    instr = imem_rdat;
  end : _fetch_regs_instr

  //------------------------------------------
  // RF writeback, reset in a software handler
  //------------------------------------------
  edubos5_rf #(
      .TARGET_IS_ASIC (TARGET_IS_ASIC)
  ) u_rf (
      .clk      (clk),                    //i
      .rs1_addr (instr.grp.reg2reg.rs1),  //i
      .rs2_addr (instr.grp.reg2reg.rs2),  //i
      .rd_addr  (instr.grp.reg2reg.rd),   //i
      .rf_we    (rf_we),                  //i

      .rf_wdat (rf_wdat),  //o
      .rs1     (rs1),      //o
      .rs2     (rs2)       //o
  );

  //----------------------------
  // Load/Store signals and data
  //----------------------------

  cpu_addr_t ls_offset;
  cpu_data_t load_to_reg, dmem_wdat_next, dmem_addr_next;
  we_bs_t dmem_we_next;

  cpu_data_t rs1_reg, rs2_reg;
  cpu_data_t ls_offset_reg;

  always_ff @(posedge clk) begin
    if (go2exec) begin
      rs1_reg       <= rs1;
      rs2_reg       <= rs2;
      ls_offset_reg <= ls_offset;
    end
  end

  always_comb begin : _load_store

    //setting up the address to read/write
    ls_offset      = instr_decode.LOAD ? cpu_addr_t'(imm_iload) : cpu_addr_t'(imm_store);

    //strobe the memory
    dmem_strobe    = instr_decode_reg.LOAD | instr_decode_reg.STORE;

    //drive the write-enable signal
    dmem_we_next   = NOWR;
    dmem_wdat_next = rs2_reg;
    dmem_addr_next = cpu_addr_t'(ls_offset_reg + cpu_addr_t'(rs1_reg));
    //differentiating between STORE instructions
    if (instr_decode_reg.STORE) begin
      unique case (funct3_store_t'(funct3_imm))
        SB: begin
          dmem_wdat_next = {4{rs2_reg[7:0]}};

          unique case (dmem_addr_next[1:0])  //see edubos_pkg.sv for definitions
            2'b00: begin
              dmem_we_next = BYTE1;
            end
            2'b01: begin
              dmem_we_next = BYTE2;
            end
            2'b10: begin
              dmem_we_next = BYTE3;
            end
            2'b11: begin
              dmem_we_next = BYTE4;
            end
            default: ;
          endcase
        end
        SH: begin
          dmem_wdat_next = {2{rs2_reg[15:0]}};

          unique case (dmem_addr_next[1])
            1'b0: begin
              dmem_we_next = HALFWORD1;
            end
            1'b1: begin
              dmem_we_next = HALFWORD2;
            end
            default: ;
          endcase
        end
        SW:      dmem_we_next = WORD;
        default: dmem_we_next = NOWR;
      endcase
    end
    //differentiating between load instructions
    load_to_reg = dmem_rdat_reg;
    unique case (funct3_imm)
      LB: begin
        unique case (dmem_addr[1:0])
          2'b00:   load_to_reg = {{24{dmem_rdat_reg[7]}}, dmem_rdat_reg[7:0]};
          2'b01:   load_to_reg = {{24{dmem_rdat_reg[15]}}, dmem_rdat_reg[15:8]};
          2'b10:   load_to_reg = {{24{dmem_rdat_reg[23]}}, dmem_rdat_reg[23:16]};
          2'b11:   load_to_reg = {{24{dmem_rdat_reg[31]}}, dmem_rdat_reg[31:24]};
          default: ;
        endcase
      end
      LH: begin
        unique case (dmem_addr[1])
          1'b0:    load_to_reg = {{16{dmem_rdat_reg[15]}}, dmem_rdat_reg[15:0]};
          1'b1:    load_to_reg = {{16{dmem_rdat_reg[31]}}, dmem_rdat_reg[31:16]};
          default: ;
        endcase
      end
      LW:      load_to_reg = dmem_rdat_reg;
      LBU: begin
        unique case (dmem_addr[1:0])
          2'b00:   load_to_reg = {{24{1'b0}}, dmem_rdat_reg[7:0]};
          2'b01:   load_to_reg = {{24{1'b0}}, dmem_rdat_reg[15:8]};
          2'b10:   load_to_reg = {{24{1'b0}}, dmem_rdat_reg[23:16]};
          2'b11:   load_to_reg = {{24{1'b0}}, dmem_rdat_reg[31:24]};
          default: ;
        endcase
      end
      LHU: begin
        unique case (dmem_addr[1])
          1'b0:    load_to_reg = {{16{1'b0}}, dmem_rdat_reg[15:0]};
          1'b1:    load_to_reg = {{16{1'b0}}, dmem_rdat_reg[31:16]};
          default: ;
        endcase
      end
      default: load_to_reg = dmem_rdat_reg;
    endcase
  end : _load_store

  //--------------
  // Branch detect
  //--------------
  logic take_brnch;
  always_comb begin : _branch
    unique case (funct3_brnch)
      BEQ:     take_brnch = eq;
      BNE:     take_brnch = ~eq;
      BLT:     take_brnch = lt;
      BGE:     take_brnch = ~lt;
      BLTU:    take_brnch = ltu;
      BGEU:    take_brnch = ~ltu;
      default: take_brnch = eq;
    endcase
  end : _branch

  //-----------------------------------------------------
  // JUMP and Program counter control
  //-----------------------------------------------------

  always_comb begin : _next_pc
    inc_pc   = pc + cpu_pc_t'(1);

    pc_auipc = pc + imm_uload_reg[31:2];
    pc_imm   = pc + (instr[3] ? imm_jump[31:2] : imm_brnch[31:2]);

    pc_jump  = |{instr_decode_reg.JAL, instr_decode_reg.BRNCH & take_brnch, instr_decode_reg.JALR};

    unique if (instr_decode_reg.JAL | (instr_decode_reg.BRNCH & take_brnch)) begin
      next_pc = pc_imm_reg;
    end
    else if (instr_decode_reg.JALR) begin
      next_pc = cpu_pc_t'(alu_out[31:2]);
    end
    else begin
      next_pc = inc_pc;
    end
  end : _next_pc

  always_comb begin : _rf_writeback
    rf_we = (state == FETCH_INSTR) & (instr.grp.reg2reg.rd != '0)  //RF[0] is not writable!
    & |{instr_decode_reg.ALUIMM, instr_decode_reg.ALUREG,
              instr_decode_reg.JALR, instr_decode_reg.JAL,
              instr_decode_reg.AUIPC, instr_decode_reg.LUI, instr_decode_reg.CNT,
              instr_decode_reg.LOAD & dmem_rdy_reg};  //Dont change to FETCH_INSTR
    unique if (instr_decode_reg.JAL | instr_decode_reg.JALR) begin
      rf_wdat = {inc_pc, 2'd0};  // JUMP: Save return PC address
    end
    else if (instr_decode_reg.AUIPC) begin
      rf_wdat = {pc_auipc, 2'b0};
    end
    else if (instr_decode_reg.LUI) begin
      rf_wdat = imm_uload;  //KEEP AS IS, affects timing
    end
    else if (instr_decode_reg.ALUIMM | instr_decode_reg.ALUREG) begin
      rf_wdat = alu_md_out;  // ALU*: Save the result of ALU operation (or M)
    end
    else begin
      rf_wdat = load_to_reg;  // LOAD: Save the data read from memory, and other (CNT: funct3 as LW)
    end

  end : _rf_writeback

  //-------------------------------------------------
  // Execution FSM
  //-------------------------------------------------

  always_ff @(negedge arst_n or posedge clk) begin : _fsm
    if (arst_n == LO) begin
      pc       <= '0;
      state    <= EXECUTE;
      imem_vld <= '1;
    end
    else if (!store_stall) begin
      unique case (state)

        FETCH_INSTR: begin
          // IMEM expected to always output in one cycle
          // imem_rdy used just for reading to dmem_rdat from imem_rdat
          pc <= next_pc;
          if (EN_WAIT4REGS == 1) begin
            state    <= pc_jump ? FETCH_TARGET : WAIT4REGS;
            imem_vld <= 1'b0;
          end
          else begin
            state    <= EXECUTE;
            imem_vld <= 1'b1;
          end
        end

        FETCH_TARGET: begin  // 3-stage: the RAM reads the jump target, now in pc
          state <= WAIT4REGS;
        end

        WAIT4REGS: begin  // MCP=2 for the fetched data
          imem_vld <= 1'b1;
          state    <= EXECUTE;
        end

        LOAD_DATA: begin  // data is loaded in the execute cycle

          imem_vld <= 1'b1;
          // eduSOC compatibility, data is valid for one cycle only in some cases (UART)
          if (dmem_rdy) begin
            state <= FETCH_INSTR;
          end
          else begin
            state <= WAIT_DATA;
          end
        end

        WAIT_DATA: begin

          if (dmem_rdy) begin  // wait for data to be ready
            state <= FETCH_INSTR;
          end
          else begin
            state <= WAIT_DATA;
          end
        end

        EXECUTE: begin

          if (instr_decode.LOAD & ((!dmem_rdy & EN_WAIT4REGS == 1) | EN_WAIT4REGS == 0)) begin
            state <= LOAD_DATA;
          end
          else begin
            if (!instr_decode.SYSTEM & !md_hold) begin
              state    <= FETCH_INSTR;
              imem_vld <= 1'b0;
            end
          end
        end

        default: state <= FETCH_INSTR;
      endcase
    end
  end : _fsm



  //-------------------------------------------------
  // Pipeline register load enable
  //-------------------------------------------------
  generate
    if (EN_WAIT4REGS == 1) begin : g_ce_3stg
      // A flop, high in the 1st EXECUTE cycle: the pipeline registers load at
      // the edge that ends it, the 2nd after the instruction arrives
      // (edubos5.3STG-ONLY.mcp.tcl relies on it). EXECUTE is entered from
      // WAIT4REGS or reset. A SYSTEM instruction stays in EXECUTE (a halt)
      // with go2exec low: its sources are frozen, so nothing needs to reload.
      always_ff @(negedge arst_n or posedge clk) begin
        if (arst_n == LO) begin
          go2exec <= 1'b1;
        end
        else begin
          go2exec <= (state == WAIT4REGS) & ~store_stall;
        end
      end
    end
    else begin : g_ce_2stg
      assign go2exec = 1'b1;  // no flop: load every clock
    end
  endgenerate

  //-------------------------------------------------
  // The ALU
  //-------------------------------------------------

  always_comb begin : _alu_inputs
    aluin_1 = rs1;

    // aluin_2 mux
    if (instr_decode.ALUREG | instr_decode.BRNCH) begin
      shamt   = rs2[4:0];
      aluin_2 = rs2;
    end
    else begin
      shamt   = instr.grp.imm.imm11_0[4:0];
      aluin_2 = imm_iload;
    end

  end : _alu_inputs

  edubos5_alu #(
      .GOWIN_FAST_OP (GOWIN_FAST_OP),     //0 for LUT sub/add, 1 for DSP sub/add
      .SHIFT_PRE_REG (EN_WAIT4REGS == 1)  //3-stage: shift before the register
  ) u_alu (
      .clk            (clk),                         //i
      .arst_n         (arst_n),                      //i
      .reset          (1'b0),                        //i
      .ce             (go2exec),                     //i
      .aluin_1        (aluin_1),                     //i
      .aluin_2        (aluin_2),                     //i
      .shamt          (shamt),                       //i
      .aluimm_dec     (instr_decode_reg.ALUIMM),     //i
      .jalr_dec       (instr_decode_reg.JALR),       //i
      .funct7_5       (funct7_alu),                  //i
      .funct7_5_instr (instr.grp.reg2reg.func7[5]),  //i
      .funct3         (instr.grp.reg2reg.func3),     //i
      .alu_out        (alu_out),                     //o
      .eq             (eq),                          //o
      .ltu            (ltu),                         //o
      .lt             (lt)                           //o
  );

  //-------------------------------------------------
  // RV32M: multiply and divide (USE_HW_MUL)
  //-------------------------------------------------
  generate
    if (USE_HW_MUL == 1'b1) begin : g_md
      // An M instruction stays in EXECUTE for a fixed count of clocks, the same
      // for any operands (constant time), then retires in FETCH_INSTR like an
      // ALU one. md_cnt counts its EXECUTE clocks from 0; at 0 the operands
      // load into rs1_reg, rs2_reg and the funct3 into funct3_imm.
      localparam int MUL_END = 6;   // 4 partial products, + M and P registers
      localparam int DIV_END = 34;  // setup, 32 steps, sign

      logic       md_op;  // decode: ALUREG with funct7 0000001
      logic       md_sel;  // md_op registered: the retiring instruction is an M one
      logic       md_step;  // an M instruction in EXECUTE, not stalled
      logic       md_end;  // its last EXECUTE clock
      logic [2:0] md_f3;  // funct3, registered
      logic [5:0] md_cnt;
      cpu_data_t mul_res, div_res;

      assign md_op   = instr_decode.ALUREG & (instr.grp.reg2reg.func7 == SEL_MULDIV);
      assign md_f3   = funct3_imm;
      assign md_step = (state == EXECUTE) & md_op & ~store_stall;
      assign md_end  = (md_cnt == (md_f3[2] ? 6'(DIV_END) : 6'(MUL_END)));
      assign md_hold = (state == EXECUTE) & md_op & ~md_end;

      always_ff @(negedge arst_n or posedge clk) begin : _md_cnt
        if (arst_n == LO) begin
          md_cnt <= '0;
        end
        else if (md_step) begin
          md_cnt <= md_end ? '0 : md_cnt + 6'd1;
        end
      end : _md_cnt

      always_ff @(negedge arst_n or posedge clk) begin : _md_sel
        if (arst_n == LO) begin
          md_sel <= 1'b0;
        end
        else if (go2exec) begin
          md_sel <= md_op;
        end
      end : _md_sel

      //--- MUL, MULH, MULHSU, MULHU --------------------------------------
      // One signed 33x33 multiply, each operand sign- or zero-extended by
      // funct3, as four 17-bit partial products through one DSP48E1 (25x18),
      // one per clock, summed in its post-adder (P = Z + M):
      //   p1 = al*bl, p2 = (p1 >>> 17) + al*bh, p3 = p2 + ah*bl,
      //   p4 = (p3 >>> 17) + ah*bh
      // al, bl: the low 17 bits (unsigned); ah, bh: the high 16 (signed).
      // Z is 0, P or P >>> 17 (OPMODE), so the sum needs no fabric adder; the
      // low bits are kept as they shift out: product {p4[29:0], p3[16:0],
      // p1[16:0]}. The DSP registers its operands (AREG, BREG), product (MREG),
      // OPMODE and sum (PREG). md_cnt: 1-4 operands in, 2-5 products, 3-6 sums.
      // p1 clears the old sum, so nothing from before counts.
      typedef enum logic [2:0] {
        Z_ZERO = 3'b000,
        Z_P    = 3'b010,
        Z_P17  = 3'b110   // P >>> 17
      } mul_z_t;

      logic mul_ce;
      logic a_sx, b_sx;  // 33rd operand bit: the sign, or 0
      logic signed [24:0] mul_a;  // to the A register: al, al, ah, ah
      logic signed [17:0] mul_b;  // to the B register: bl, bh, bl, bh
      mul_z_t             mul_z;  // to the OPMODE register: for the product
      logic [47:0] mul_p;  // the P register
      logic [16:0] mul_lo, mul_mid;  // p1[16:0], p3[16:0]
      logic [17:0] NC_mul_p;  // above bit 63 of the product
      assign       NC_mul_p = mul_p[47:30];

      assign mul_ce   = md_step & ~md_f3[2];
      assign a_sx     = rs1_reg[31] & (md_f3[1:0] != 2'b11);  // MULH, MULHSU (MUL: any)
      assign b_sx     = rs2_reg[31] & (md_f3[1:0] == 2'b01);  // MULH
      assign mul_a    = (md_cnt >= 6'd3) ? {{10{a_sx}}, rs1_reg[31:17]} : {8'd0, rs1_reg[16:0]};
      assign mul_b    = md_cnt[0]
                      ? {1'b0, rs2_reg[16:0]}
                      : {{3{b_sx}}, rs2_reg[31:17]};

      always_comb begin : _mul_z
        unique case (md_cnt)
          6'd3, 6'd5: mul_z = Z_P17;
          6'd4:       mul_z = Z_P;
          default:    mul_z = Z_ZERO;  // md_cnt 2: p1
        endcase
      end : _mul_z

      always_ff @(posedge clk) begin : _mul_bits
        if (mul_ce & (md_cnt == 6'd4)) begin
          mul_lo <= mul_p[16:0];
        end
        if (mul_ce & (md_cnt == 6'd6)) begin
          mul_mid <= mul_p[16:0];
        end
      end : _mul_bits

      if (USE_DSP48E1 == 1'b1) begin : g_dsp48e1
        // the Xilinx 7-series primitive: A/B, M, OPMODE and P registers
        logic [29:0] NC_acout;
        logic [17:0] NC_bcout;
        logic [47:0] NC_pcout;
        logic [ 3:0] NC_carryout;
        logic NC_carrycascout, NC_multsignout, NC_overflow, NC_underflow;
        logic NC_patterndetect, NC_patternbdetect;

        logic [29:0] mul_a_dsp;  // A: 30 bits, mul_a sign-extended
        assign       mul_a_dsp = {{5{mul_a[24]}}, mul_a};

        DSP48E1 #(
            .A_INPUT       ("DIRECT"),
            .B_INPUT       ("DIRECT"),
            .USE_DPORT     ("FALSE"),
            .USE_MULT      ("MULTIPLY"),
            .USE_SIMD      ("ONE48"),
            .AREG          (1),
            .ACASCREG      (1),
            .BREG          (1),
            .BCASCREG      (1),
            .MREG          (1),
            .PREG          (1),
            .OPMODEREG     (1),
            .ALUMODEREG    (1),
            .INMODEREG     (1),
            .CARRYINREG    (1),
            .CARRYINSELREG (1),
            .CREG          (0),
            .DREG          (0),
            .ADREG         (0)
        ) u_dsp (
            .CLK            (clk),
            .A              (mul_a_dsp),
            .B              (mul_b),
            .C              (48'd0),
            .D              (25'd0),
            .OPMODE         ({mul_z, 4'b0101}),  // X = M, Y = M
            .ALUMODE        (4'b0000),           // Z + X + Y + CIN
            .INMODE         (5'b00000),          // A2 * B2
            .CARRYIN        (1'b0),
            .CARRYINSEL     (3'b000),
            .CEA1           (1'b0),
            .CEA2           (mul_ce),
            .CEB1           (1'b0),
            .CEB2           (mul_ce),
            .CEM            (mul_ce),
            .CEP            (mul_ce),
            .CECTRL         (mul_ce),
            .CEALUMODE      (1'b1),
            .CEINMODE       (1'b1),
            .CECARRYIN      (1'b1),
            .CEC            (1'b0),
            .CED            (1'b0),
            .CEAD           (1'b0),
            .RSTA           (1'b0),
            .RSTB           (1'b0),
            .RSTM           (1'b0),
            .RSTP           (1'b0),
            .RSTC           (1'b0),
            .RSTD           (1'b0),
            .RSTCTRL        (1'b0),
            .RSTALUMODE     (1'b0),
            .RSTINMODE      (1'b0),
            .RSTALLCARRYIN  (1'b0),
            .ACIN           (30'd0),
            .BCIN           (18'd0),
            .PCIN           (48'd0),
            .CARRYCASCIN    (1'b0),
            .MULTSIGNIN     (1'b0),
            .P              (mul_p),             //o
            .ACOUT          (NC_acout),          //o
            .BCOUT          (NC_bcout),          //o
            .PCOUT          (NC_pcout),          //o
            .CARRYOUT       (NC_carryout),       //o
            .CARRYCASCOUT   (NC_carrycascout),   //o
            .MULTSIGNOUT    (NC_multsignout),    //o
            .OVERFLOW       (NC_overflow),       //o
            .UNDERFLOW      (NC_underflow),      //o
            .PATTERNDETECT  (NC_patterndetect),  //o
            .PATTERNBDETECT (NC_patternbdetect)  //o
        );
      end : g_dsp48e1
      else begin : g_mul_rtl
        // portable: the same registers and timing in plain RTL
        logic signed [24:0] mul_a_reg;
        logic signed [17:0] mul_b_reg;
        logic signed [42:0] mul_m;
        mul_z_t             mul_z_reg;

        // OPMODE: selects the sum, reset
        always_ff @(negedge arst_n or posedge clk) begin : _mul_z_reg
          if (arst_n == LO) begin
            mul_z_reg <= Z_ZERO;
          end
          else if (mul_ce) begin
            mul_z_reg <= mul_z;
          end
        end : _mul_z_reg

        always_ff @(posedge clk) begin : _mul
          if (mul_ce) begin
            mul_a_reg <= mul_a;
            mul_b_reg <= mul_b;
            mul_m     <= mul_a_reg * mul_b_reg;
            unique case (mul_z_reg)
              Z_P:     mul_p <= mul_p + 48'(mul_m);
              Z_P17:   mul_p <= ($signed(mul_p) >>> 17) + 48'(mul_m);
              default: mul_p <= 48'(mul_m);
            endcase
          end
        end : _mul
      end : g_mul_rtl

      always_comb begin : _mul_res
        if (md_f3[1:0] == 2'b00) begin
          mul_res = {mul_mid[14:0], mul_lo};  // MUL: the low word
        end
        else begin
          mul_res = {mul_p[29:0], mul_mid[16:15]};
        end
      end : _mul_res

      //--- DIV, DIVU, REM, REMU ------------------------------------------
      // Restoring division of the magnitudes, one quotient bit per clock,
      // always 32 steps, then the signs. The ISA corner cases need no special
      // path: a zero divisor gives quotient all ones and remainder the
      // dividend (the quotient is not negated then), INT_MIN / -1 gives INT_MIN
      // and remainder 0. The last clock writes the result into quo.
      logic div_ce;
      logic n_sx, d_sx;  // signed DIV, REM: the operand is negative
      logic q_neg, r_neg;
      logic [31:0] quo, rem, dvs;
      logic [32:0] r_sh;  // the remainder shifted, with the next dividend bit
      logic [33:0] r_dif;  // r_sh - dvs: negative when dvs does not fit

      assign div_ce  = md_step & md_f3[2];
      assign n_sx    = rs1_reg[31] & ~md_f3[0];
      assign d_sx    = rs2_reg[31] & ~md_f3[0];
      assign r_sh    = {rem, quo[31]};
      assign r_dif   = {1'b0, r_sh} - {2'b00, dvs};
      assign div_res = quo;

      logic  NC_r_dif;  // 0 unless dvs is 0: a remainder is below dvs
      assign NC_r_dif = r_dif[32];

      // the result signs: they select, reset
      always_ff @(negedge arst_n or posedge clk) begin : _div_signs
        if (arst_n == LO) begin
          q_neg <= 1'b0;
          r_neg <= 1'b0;
        end
        else if (div_ce & (md_cnt == 6'd1)) begin  // setup
          q_neg <= (n_sx ^ d_sx) & (|rs2_reg);
          r_neg <= n_sx;
        end
      end : _div_signs

      always_ff @(posedge clk) begin : _div
        if (div_ce) begin
          if (md_cnt == 6'd1) begin  // setup
            quo <= n_sx ? -rs1_reg : rs1_reg;
            dvs <= d_sx ? -rs2_reg : rs2_reg;
            rem <= '0;
          end
          else if (md_cnt == 6'(DIV_END)) begin  // the signs
            quo <= md_f3[1] ? (r_neg ? -rem : rem) : (q_neg ? -quo : quo);
          end
          else if (md_cnt != '0) begin  // a step
            rem <= r_dif[33] ? r_sh[31:0] : r_dif[31:0];
            quo <= {quo[30:0], ~r_dif[33]};
          end
        end
      end : _div

`ifdef RISCV_FORMAL_ALTOPS
      // riscv-formal's alternative M results (formal only): its checks then
      // see decode, timing and write-back, not the arithmetic, which a SAT
      // solver cannot prove at 32 bits
      cpu_data_t alt_res;
      cpu_data_t NC_md_res;
      assign NC_md_res = mul_res ^ div_res;

      always_comb begin : _altops
        unique case (md_f3)
          3'b000:  alt_res = (rs1_reg + rs2_reg) ^ 32'h5876_063e;
          3'b001:  alt_res = (rs1_reg + rs2_reg) ^ 32'hf658_3fb7;
          3'b010:  alt_res = (rs1_reg - rs2_reg) ^ 32'hecfb_e137;
          3'b011:  alt_res = (rs1_reg + rs2_reg) ^ 32'h949c_e5e8;
          3'b100:  alt_res = (rs1_reg - rs2_reg) ^ 32'h7f85_29ec;
          3'b101:  alt_res = (rs1_reg - rs2_reg) ^ 32'h10e8_fd70;
          3'b110:  alt_res = (rs1_reg - rs2_reg) ^ 32'h8da6_8fa5;
          3'b111:  alt_res = (rs1_reg - rs2_reg) ^ 32'h3138_d0e1;
          default: alt_res = '0;
        endcase
      end : _altops

      assign alu_md_out = md_sel ? alt_res : alu_out;
`else
      assign alu_md_out = md_sel ? (md_f3[2] ? div_res : mul_res) : alu_out;
`endif
    end : g_md
    else begin : g_no_md
      assign md_hold    = 1'b0;
      assign alu_md_out = alu_out;
    end : g_no_md
  endgenerate

  //---------------------------------------------------------------------
  // Flopping the interface to dmem: avoid circular logic, increase Fmax
  //---------------------------------------------------------------------


  logic dmem_vld_next;
  assign dmem_vld_next = (|EN_WAIT4REGS & state != WAIT4REGS | ~|EN_WAIT4REGS)
                       & (instr_decode_reg.LOAD & (state == LOAD_DATA | state == WAIT_DATA)
                            & ~dmem_rdy
                          | instr_decode_reg.STORE & state == FETCH_INSTR);

  assign store_stall = STORE_WAITS_RDY & dmem_vld & (dmem_we != NOWR) & ~dmem_rdy;

  // the request and its write strobes: reset, as store_stall feeds back on them
  always_ff @(negedge arst_n or posedge clk) begin : _dmem_vld
    if (arst_n == LO) begin
      dmem_vld <= 1'b0;
      dmem_we  <= NOWR;
    end
    else if (!store_stall) begin
      dmem_vld <= dmem_vld_next;
      dmem_we  <= we_bs_t'(dmem_we_next & {4{state == FETCH_INSTR}});
    end
  end : _dmem_vld

  always_ff @(negedge arst_n or posedge clk) begin : _dmem_rdy
    if (arst_n == LO) begin
      dmem_rdy_reg <= 1'b0;
    end
    else begin
      dmem_rdy_reg <= dmem_rdy;
    end
  end : _dmem_rdy

  always_ff @(posedge clk) begin : _dmem_connection
    // data gating; a waiting STORE keeps its request
    if (!store_stall) begin
      dmem_addr <= dmem_addr_next;
      if (state == FETCH_INSTR) begin
        dmem_wdat <= dmem_wdat_next;
      end
    end
    if (state == WAIT_DATA) begin
      dmem_rdat_reg <= dmem_rdat;
    end
    else if (instr_decode.CNT & (state == EXECUTE)) begin
      // a counter read, written back whole, as a LW (funct3 01x or 11x): no
      // new input on the timing-critical rf_wdat mux
      dmem_rdat_reg <= cnt_rdat;
    end
  end : _dmem_connection

  // MCP capture side: the registers the sources feed (pc and the FSM load
  // nothing from them at the 1st edge either)
  `define EDUBOS5_MCP_CAPTURE {instr_decode_reg, funct3_brnch, funct7_alu, funct3_imm, \
      pc_imm_reg, imm_uload_reg, rs1_reg, rs2_reg, ls_offset_reg, pc, dmem_rdat_reg, \
      u_alu.shifter, u_alu.aluin_1_31_reg, u_alu.aluin_2_31_reg, \
      u_alu.alu_and_reg, u_alu.alu_xor_reg, u_alu.alu_or_reg, u_alu.funct3_alu_reg, \
      u_alu.alu_plus_reg, u_alu.alu_minus_reg}

`ifdef EDUBOS5_MCP_CHECK
  // Proof of edubos5.3STG-ONLY.mcp.tcl (simulation; 5.sim, CORE=edubos5_3stg):
  // the instruction port and the register file reads change only on a fetch
  // (FETCH_INSTR, FETCH_TARGET), hold 2 clocks from the last one, and their
  // paths get 2 cycles.
  logic mcp_started;  // checked from the first FETCH_INSTR on
  always_ff @(negedge arst_n or posedge clk) begin
    if (arst_n == LO) begin
      mcp_started <= 1'b0;
    end
    else if (state == FETCH_INSTR) begin
      mcp_started <= 1'b1;
    end
  end

  // A source changes only on a fetch edge, the one that ends FETCH_INSTR or
  // FETCH_TARGET. The launch edge is the last of them (not into FETCH_TARGET),
  // and the source holds until the capture 2 clocks later
  // verilog_format: off
  a_mcp_launch_only_at_fetch :
  assert property (@(posedge clk) disable iff (!arst_n || !mcp_started)
      $changed({imem_rdat, rs1, rs2}) |-> $past(state) inside {FETCH_INSTR, FETCH_TARGET})
  else begin
    $error("EDUBOS5_MCP_CHECK: source changed outside the launch edge, pc %h", {pc, 2'b00});
  end

  a_mcp_held_2_clocks :
  assert property (@(posedge clk) disable iff (!arst_n || !mcp_started)
      $past(state) inside {FETCH_INSTR, FETCH_TARGET} && state != FETCH_TARGET
      |=> $stable({imem_rdat, rs1, rs2}))
  else begin
    $error("EDUBOS5_MCP_CHECK: source not held 2 clocks after launch, pc %h", {pc, 2'b00});
  end

  // Nothing the sources feed loads at the 1st edge after launch
  a_mcp_capture_at_2nd_edge :
  assert property (@(posedge clk) disable iff (!arst_n || !mcp_started)
      $past(state) inside {FETCH_INSTR, FETCH_TARGET} && state != FETCH_TARGET
      |=> $stable(`EDUBOS5_MCP_CAPTURE))
  else begin
    $error("EDUBOS5_MCP_CHECK: a register loaded at the 1st edge after launch, pc %h", {pc, 2'b00});
  end
  // verilog_format: on

`endif

`ifdef EDUBOS5_MCP_FORMAL
  // The same properties for a static proof (formal/run-mcp-formal.sh):
  // open-source formal front ends take only immediate assertions, and Yosys
  // read_slang only $past, not $changed / $stable.
  logic fv_on;  // checked from the clock after reset on
  always_ff @(negedge arst_n or posedge clk) begin
    if (arst_n == LO) begin
      fv_on <= 1'b0;
    end
    else begin
      fv_on <= 1'b1;
    end
  end

  always_ff @(posedge clk) begin
    if ($past(fv_on)) begin
      assert ({imem_rdat, rs1, rs2} == $past({imem_rdat, rs1, rs2}) ||
              $past(state) == FETCH_INSTR || $past(state) == FETCH_TARGET);
    end
    // the load enable: 2-stage always; 3-stage only in EXECUTE, and always
    // on entering it from WAIT4REGS
    if (fv_on) begin
      assert (EN_WAIT4REGS == 1 || go2exec);
      assert (EN_WAIT4REGS == 0 || !go2exec || state == EXECUTE);
    end
    if ($past(fv_on) && $past(state) == WAIT4REGS && state == EXECUTE) begin
      assert (go2exec);
    end
    // 2 clocks after the launch edge, the last fetch edge
    if ($past(
            fv_on, 2
        ) && ($past(
            state, 2
        ) == FETCH_INSTR || $past(
            state, 2
        ) == FETCH_TARGET) && $past(
            state
        ) != FETCH_TARGET) begin
      assert ({imem_rdat, rs1, rs2} == $past({imem_rdat, rs1, rs2}));
      assert (`EDUBOS5_MCP_CAPTURE == $past(`EDUBOS5_MCP_CAPTURE));
    end
  end
`endif
  `undef EDUBOS5_MCP_CAPTURE

`ifdef RISCV_FORMAL
  // An instruction retires in FETCH_INSTR: the register file is written there,
  // while imem_rdat still holds the instruction, pc its address, rs1_reg and
  // rs2_reg its operands, and the load/store logic its memory access. A
  // SYSTEM instruction halts the core in EXECUTE and never retires. No traps,
  // no interrupts; machine mode, 32-bit. Memory: word-aligned address, masks.
  logic rvfi_rs1_used, rvfi_rs2_used;

  always_comb begin : _rvfi
    rvfi_valid = (state == FETCH_INSTR);
    rvfi_insn = imem_rdat;
    rvfi_trap = 1'b0;
    rvfi_halt = 1'b0;
    rvfi_intr = 1'b0;
    rvfi_mode = 2'd3;
    rvfi_ixl = 2'd1;

    rvfi_rs1_used  = |{instr_decode_reg.ALUREG, instr_decode_reg.ALUIMM, instr_decode_reg.BRNCH,
                       instr_decode_reg.JALR, instr_decode_reg.LOAD, instr_decode_reg.STORE};
    rvfi_rs2_used = |{instr_decode_reg.ALUREG, instr_decode_reg.BRNCH, instr_decode_reg.STORE};
    rvfi_rs1_addr = rvfi_rs1_used ? instr.grp.reg2reg.rs1 : 5'd0;  // 0: not read
    rvfi_rs2_addr = rvfi_rs2_used ? instr.grp.reg2reg.rs2 : 5'd0;
    rvfi_rs1_rdata = rvfi_rs1_used ? rs1_reg : '0;
    rvfi_rs2_rdata = rvfi_rs2_used ? rs2_reg : '0;
    rvfi_rd_addr = rf_we ? instr.grp.reg2reg.rd : 5'd0;  // 0: no register written
    rvfi_rd_wdata = rf_we ? rf_wdat : '0;
    rvfi_pc_rdata = {pc, 2'b00};
    rvfi_pc_wdata = {next_pc, 2'b00};

    rvfi_mem_addr = '0;
    rvfi_mem_rmask = '0;
    rvfi_mem_wmask = '0;
    rvfi_mem_rdata = '0;
    rvfi_mem_wdata = '0;
    if (instr_decode_reg.LOAD) begin
      rvfi_mem_addr  = {dmem_addr[31:2], 2'b00};
      rvfi_mem_rdata = dmem_rdat_reg;
      unique case (funct3_imm)
        LB, LBU: rvfi_mem_rmask = 4'b0001 << dmem_addr[1:0];
        LH, LHU: rvfi_mem_rmask = 4'b0011 << {dmem_addr[1], 1'b0};
        default: rvfi_mem_rmask = 4'b1111;
      endcase
    end
    if (instr_decode_reg.STORE) begin
      rvfi_mem_addr  = {dmem_addr_next[31:2], 2'b00};
      rvfi_mem_wmask = dmem_we_next;
      rvfi_mem_wdata = dmem_wdat_next;
    end
  end : _rvfi

  // order: the number of instructions retired before this one
  always_ff @(negedge arst_n or posedge clk) begin : _rvfi_order
    if (arst_n == LO) begin
      rvfi_order <= '0;
    end
    else if (rvfi_valid) begin
      rvfi_order <= rvfi_order + 64'd1;
    end
  end : _rvfi_order

  // EN_COUNTERS: a counter read returns the counter of its EXECUTE clock, the
  // one before it retires: cycle is the clocks since reset less 1, instret the
  // instructions retired before it (rvfi_order). Insn bit 21: instret; 27: the
  // high half. riscv-formal has no Zicntr check of its own
  logic [63:0] fv_cycle, fv_cnt;
  assign       fv_cnt = rvfi_insn[21] ? rvfi_order : fv_cycle - 64'd1;

  always_ff @(negedge arst_n or posedge clk) begin : _fv_cnt
    if (arst_n == LO) begin
      fv_cycle <= '0;
    end
    else begin
      fv_cycle <= fv_cycle + 64'd1;
      if (EN_COUNTERS && rvfi_valid && instr_decode_reg.CNT && rvfi_rd_addr != 5'd0) begin
        assert (rvfi_rd_wdata == (rvfi_insn[27] ? fv_cnt[63:32] : fv_cnt[31:0]));
      end
    end
  end : _fv_cnt
`endif

endmodule : edubos5

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Aug 9, 2023
// -----------------------------------------------------------------------------
