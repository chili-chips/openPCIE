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
// Description:
//   The SOC alone (soc_top: eduBOS5, RAM, CSR, the TLP payload RAMs) with a
//   stand-in for the PCIE stack, on Verilator. From openCologne-PCIE's
//   2.rtl/7.soc/sim/tb_soc.sv. run-soc.sh builds the firmware of a test in
//   3.sw/tests/ and picks the check:
//     +tx_check   soc_tx: every s_axis_tx beat of 3-DW and 4-DW requests,
//                 single and batched, from even and odd entries
//     +rx_check   soc_rx: RX_TLPS TLPs back to back, more than the RX ring
//                 holds; the firmware sends each back as it came
//     +cpl_check  soc_cpl: the RC request layer (3.sw/common/pcie.c): a
//                 32-DW burst read answered by two 16-DW CplDs, behind a
//                 Message and a stale Completion it must free unread; a
//                 1-DW read answered with UR
//   Every test ends with the firmware's result marker: 0x0000FACE = PASS.
//==========================================================================

module tb_soc
  import pcie_pkg::*;
;
  logic clk = 1'b0;
  logic resetn = 1'b0;

  always #8ns clk = ~clk;  // 62.5 MHz, the x1 user clock

  initial begin
    repeat (10) @(posedge clk);
    resetn = 1'b1;
  end

  axis_if #(
      .DATA_W (TRN_DATA_W),
      .USER_W ($bits(pcie_tx_tuser_t))
  ) s_axis_tx ();
  axis_if #(
      .DATA_W (TRN_DATA_W),
      .USER_W ($bits(pcie_rx_tuser_t))
  ) m_axis_rx ();
  cfg_if cfg ();

  // the PCIE stack stand-in: takes every TLP, has transmit buffers free. It
  // stalls as the core's throttle does: tready low one clock in three, and
  // four clocks in a row every sixteen, so beats wait in the Tx FIFO. The
  // pattern advances on clocks with a beat only, so the first beat of every
  // run waits: a short test cannot slip between the stalls
  logic [3:0] tx_stall_n = '0;
  always @(posedge clk) if (s_axis_tx.tvalid) tx_stall_n <= tx_stall_n + 4'd1;
  assign s_axis_tx.tready  = (tx_stall_n % 3 != 0) & (tx_stall_n < 4'd12);

  int tx_stalls = 0;  // clocks a Tx beat waited
  always @(posedge clk) if (s_axis_tx.tvalid && !s_axis_tx.tready) tx_stalls <= tx_stalls + 1;
  assign m_axis_rx.tuser   = '0;
  assign cfg.msg_err_fatal = 1'b0;
  assign cfg.tx_buf_avail  = 6'd32;

  logic fw_pass;

  soc_top dut (
      .clk       (clk),
      .resetn    (resetn),
      .s_axis_tx (s_axis_tx),
      .m_axis_rx (m_axis_rx),
      .cfg       (cfg),
      .fw_pass   (fw_pass)
  );

  typedef struct packed {
    logic [63:0] data;  // [72:9]
    logic [7:0]  keep;  // [8:1]
    logic        last;  // [0]
  } beat_t;

  // +tx_check (soc_tx): every s_axis_tx beat of the requests that firmware
  // sends, 3-DW and 4-DW. A beat with tkeep 0F is compared on its low DW only
  localparam int NBEATS = 12;
  localparam beat_t EXP[NBEATS] = '{
      // 3-DW MWr, 1 DW
      {64'h0000010F_40000001, 8'hFF, 1'b0},
      {64'hA1B2C3D4_00001000, 8'hFF, 1'b1},
      // 4-DW MWr, 1 DW
      {64'h0000020F_60000001, 8'hFF, 1'b0},
      {64'h00002000_00000001, 8'hFF, 1'b0},
      {64'h00000000_11223344, 8'h0F, 1'b1},
      // 4-DW MWr, 2 DW
      {64'h000003FF_60000002, 8'hFF, 1'b0},
      {64'h00003000_00000002, 8'hFF, 1'b0},
      {64'h99AABBCC_55667788, 8'hFF, 1'b1},
      // 3-DW MRd, 1 DW (the batch's second)
      {64'h0000050F_00000001, 8'hFF, 1'b0},
      {64'h00000000_00005000, 8'h0F, 1'b1},
      // 4-DW MRd, 1 DW (the batch's third, from an odd entry)
      {64'h0000040F_20000001, 8'hFF, 1'b0},
      {64'h00004000_00000003, 8'hFF, 1'b1}
  };

  // +rx_check (soc_rx): RX_TLPS TLPs back to back into m_axis_rx, more than
  // the SOC's RX ring holds, with a gap after every seventh. The firmware
  // sends each one back as it came, in TX batches, so s_axis_tx must carry
  // the same beats
  localparam int RX_TLPS = 96;  // soc_rx/main.c: RX_TLPS

  beat_t rx_q[$];  // the RX beats (rx_check: also the TX beats expected)
  int    rx_n = 0;  // the next RX beat
  int    rx_t = 0;  // TLPs sent
  int    rx_gap = 0;
  int    rx_stalls = 0;  // clocks the ring held a beat off

  // one TLP, DWORD by DWORD, as beats into rx_q
  function automatic void rx_push(input logic [31:0] dw[$]);
    for (int k = 0; k < dw.size(); k += 2) begin
      rx_q.push_back('{data: {(k + 1 < dw.size()) ? dw[k+1] : 32'h0, dw[k]},
                       keep: (k + 1 < dw.size()) ? 8'hFF : 8'h0F,
                       last: k + 2 >= dw.size()});
    end
  endfunction

  function automatic void rx_tlp(int t);
    int          kind = t % 4;  // MWr3, MWr4, MRd3, MRd4
    logic [31:0] dw[$];
    int          len = (kind < 2) ? ((t * 7) % 32) + 1 : 1;
    dw.push_back({1'b0, kind < 2, kind % 2 == 1, 19'd0, 10'(len)});
    for (int k = 1; k < ((kind % 2) ? 4 : 3) + ((kind < 2) ? len : 0); k++) begin
      dw.push_back(32'hC0DE_0000 | (t << 8) | k);
    end
    rx_push(dw);
  endfunction

  // +cpl_check (soc_cpl): the answers to the firmware's requests, built when
  // a request leaves. Requester ID 0x10EE (pcie.h MY_REQUESTER_ID)
  localparam logic [15:0] RID = 16'h10EE;

  // a Completion: Fmt 10 (CplD) or 00 (Cpl), Type 01010
  function automatic void rx_cpl(input logic [7:0] tag, input logic [2:0] status,
                                 input int len, input logic [11:0] byte_count,
                                 input logic [6:0] lower_addr, input int first);
    logic [31:0] dw[$];
    dw.push_back({1'b0, len != 0, 1'b0, 5'b01010, 14'd0, 10'(len)});
    dw.push_back({16'h0100, status, 1'b0, byte_count});  // Completer ID 01:00.0
    dw.push_back({RID, tag, 1'b0, lower_addr});
    for (int i = 0; i < len; i++) dw.push_back(32'hD000_0000 | (first + i));
    rx_push(dw);
  endfunction

  logic tx_check;
  logic rx_check;
  logic cpl_check;
  int   nbeat = 0;
  int   errors = 0;
  int   cpl_reqs = 0;  // requests answered (cpl_check)
  beat_t got;

  initial begin
    tx_check  = $test$plusargs("tx_check");
    rx_check  = $test$plusargs("rx_check");
    cpl_check = $test$plusargs("cpl_check");
    if (rx_check) begin
      for (int t = 0; t < RX_TLPS; t++) rx_tlp(t);
    end
  end

  assign m_axis_rx.tvalid = resetn & (rx_n < rx_q.size()) & (rx_gap == 0);
  assign m_axis_rx.tdata  = (rx_n < rx_q.size()) ? rx_q[rx_n].data : '0;
  assign m_axis_rx.tkeep  = (rx_n < rx_q.size()) ? rx_q[rx_n].keep : '0;
  assign m_axis_rx.tlast  = (rx_n < rx_q.size()) ? rx_q[rx_n].last : 1'b0;

  always @(posedge clk) begin
    if (rx_gap != 0) begin
      rx_gap <= rx_gap - 1;
    end
    else if (m_axis_rx.tvalid && m_axis_rx.tready) begin
      rx_n <= rx_n + 1;
      if (m_axis_rx.tlast) begin
        rx_t <= rx_t + 1;
        if (rx_check && rx_t % 7 == 6) rx_gap <= 3;
      end
    end
    else if (m_axis_rx.tvalid) begin
      rx_stalls <= rx_stalls + 1;
    end
  end

  assign got = '{data: s_axis_tx.tdata, keep: s_axis_tx.tkeep, last: s_axis_tx.tlast};

  // cpl_check: DW0/DW1 of the request now leaving, kept for its answer
  logic [31:0] req_dw0, req_dw1;
  bit          req_sop = 1'b1;

  always @(posedge clk) begin
    if (rx_check && resetn && s_axis_tx.tvalid && s_axis_tx.tready) begin
      if (nbeat >= rx_q.size()) begin
        errors++;
        $display("tb_soc: ERROR beat %0d: none expected, got %h %h %b", nbeat, got.data, got.keep,
                 got.last);
      end
      else if (got.keep != rx_q[nbeat].keep || got.last != rx_q[nbeat].last ||
               got.data[31:0] != rx_q[nbeat].data[31:0] ||
               (got.keep == 8'hFF && got.data[63:32] != rx_q[nbeat].data[63:32])) begin
        if (errors < 10) begin
          $display("tb_soc: ERROR beat %0d: expected %h %h %b, got %h %h %b", nbeat,
                   rx_q[nbeat].data, rx_q[nbeat].keep, rx_q[nbeat].last, got.data, got.keep,
                   got.last);
        end
        errors++;
      end
      nbeat++;
    end
    if (tx_check && resetn && s_axis_tx.tvalid && s_axis_tx.tready) begin
      if (nbeat >= NBEATS) begin
        errors++;
        $display("tb_soc: ERROR beat %0d: none expected, got %h %h %b", nbeat, got.data, got.keep,
                 got.last);
      end
      else if (got.keep != EXP[nbeat].keep || got.last != EXP[nbeat].last ||
               got.data[31:0] != EXP[nbeat].data[31:0] ||
               (got.keep == 8'hFF && got.data[63:32] != EXP[nbeat].data[63:32])) begin
        errors++;
        $display("tb_soc: ERROR beat %0d: expected %h %h %b, got %h %h %b", nbeat,
                 EXP[nbeat].data, EXP[nbeat].keep, EXP[nbeat].last, got.data, got.keep, got.last);
      end
      nbeat++;
    end
    // cpl_check: answer each MRd when its last beat leaves. The first, 32
    // DW, gets a Message and a stale Completion first, then two 16-DW CplDs
    // split at the 64-byte RCB; the second, 1 DW, gets a Cpl with UR
    if (cpl_check && resetn && s_axis_tx.tvalid && s_axis_tx.tready) begin
      if (req_sop) begin
        req_dw0 = s_axis_tx.tdata[31:0];
        req_dw1 = s_axis_tx.tdata[63:32];
      end
      if (s_axis_tx.tlast && req_dw0[31:24] == 8'h00) begin  // a 3-DW MRd
        if (cpl_reqs == 0) begin
          logic [31:0] msg[$];
          msg = '{32'h3400_0000, {RID, 8'h00, 8'h20}, 32'h0, 32'h0};  // Msg, Assert_INTA
          rx_push(msg);
          rx_cpl(req_dw1[15:8] ^ 8'h01, 3'd0, 1, 12'd4, 7'h00, 99);  // stale: other tag
          rx_cpl(req_dw1[15:8], 3'd0, 16, 12'd128, 7'h00, 0);
          rx_cpl(req_dw1[15:8], 3'd0, 16, 12'd64, 7'h40, 16);
        end
        else begin
          rx_cpl(req_dw1[15:8], 3'd1, 0, 12'd4, 7'h00, 0);  // UR
        end
        cpl_reqs++;
      end
      req_sop = s_axis_tx.tlast;
    end
  end

  initial begin
    $dumpfile("tb_soc.fst");
    $dumpvars(1, tb_soc);
    fork  // until the firmware is done, 10 ms at most
      wait (dut.fw_result != 32'h0);
      #10ms;
    join_any
    repeat (20) @(posedge clk);  // the last TLP leaves
    $timeformat(-9, 0, " ns", 0);
    $display("tb_soc: %0t, result 0x%08h, fw_pass=%0b, Tx beats stalled %0d clocks", $realtime,
             dut.fw_result, fw_pass, tx_stalls);
    if (tx_stalls == 0) begin
      errors++;
      $display("tb_soc: ERROR no Tx beat was stalled");
    end
    if (!fw_pass) begin
      errors++;
      $display("tb_soc: ERROR the firmware did not pass (result 0x%08h)", dut.fw_result);
    end
    if (tx_check) begin
      if (nbeat != NBEATS) begin
        errors++;
        $display("tb_soc: ERROR %0d beat(s) sent, %0d expected", nbeat, NBEATS);
      end
      $display("tb_soc: Tx check, %0d error(s): TEST %s", errors, errors == 0 ? "PASSED" : "FAILED");
    end
    if (rx_check) begin
      if (nbeat != rx_q.size()) begin
        errors++;
        $display("tb_soc: ERROR %0d beat(s) sent back, %0d expected", nbeat, rx_q.size());
      end
      if (rx_stalls == 0) begin
        errors++;
        $display("tb_soc: ERROR the RX ring never filled");
      end
      $display("tb_soc: Rx check, %0d TLPs, %0d beats, ring full %0d clocks, %0d error(s): TEST %s",
               RX_TLPS, rx_q.size(), rx_stalls, errors, errors == 0 ? "PASSED" : "FAILED");
    end
    if (cpl_check) begin
      if (cpl_reqs != 2) begin
        errors++;
        $display("tb_soc: ERROR %0d request(s) answered, 2 expected", cpl_reqs);
      end
      if (dut.rx_tlps != 0) begin
        errors++;
        $display("tb_soc: ERROR %0d TLP(s) left in the RX ring", dut.rx_tlps);
      end
      $display("tb_soc: Cpl check, %0d TLPs in, %0d error(s): TEST %s", rx_t, errors,
               errors == 0 ? "PASSED" : "FAILED");
    end
    $finish;
  end

endmodule : tb_soc

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 27, 2026
// -----------------------------------------------------------------------------
