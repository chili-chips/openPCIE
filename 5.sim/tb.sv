//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   openpcie2-rc top level co-simulation test bench.
//
//   The DUT is the REAL Root Complex, 2.rtl/0.common.opensource:
//
//        RC_opensource
//          |- pcie_top
//          |    |- clk_rst_gen        MMCM, real RTL
//          |    |- pcie_core       real RTL
//          |    |    `- PCIE_2_1     Xilinx hard macro
//          |    `- phy_top     <-- SWAPPED for models/phy_top.EP-PIPE-BFM.sv
//          |                            (PIPE PHY model + pcievhost endpoint)
//          `- soc_top
//               |- picorv32 / edubos5  real RTL, runs the real firmware.hex
//               `- soc_csr -> csr   PeakRDL-generated CSR
//
//   Only two files differ from the bitstream build:
//
//     phy_top  the 7-series transceiver cannot be paired with a PIPE
//                   level VIP, so a behavioural PIPE PHY takes its place and
//                   carries the pcieVHost endpoint on its far side
//     firmware.hex  read by soc_top.sv with $readmemh, copied into the
//                   simulation directory by the Makefile
//
//   Everything else -- the hard macro, the transaction layer, the CPU, the
//   CSR and the software -- is exactly what goes into the FPGA.
//==========================================================================

module tb;

  // The run ends RESULT_TAIL_US after the firmware reports its result, so the
  // last completion still shows; RUN_SIM_US is only the time limit for a run
  // that never reports. It is read, in microseconds, at time 0 from run_us.cfg,
  // which the make file writes from its RUN_US variable, so changing it needs
  // no re-elaboration:
  //     make run RUN_US=40000
  // A file is used rather than a plusarg because xsim's -testplusarg does not
  // accept the name=value form -- it splits the argument at the '='.
  integer RUN_SIM_US;
  localparam int RESULT_TAIL_US = 10;
  event result_seen;  // the firmware has written its result marker

  // A task called from the clock-and-reset initial block, so the run length is
  // set before its time limit uses it (two initial blocks run in any order)
  task automatic get_run_length();
    integer fh;
    integer code;

    RUN_SIM_US = 2000;

    fh = $fopen("run_us.cfg", "r");
    if (fh != 0) begin
      code = $fscanf(fh, "%d", RUN_SIM_US);
      $fclose(fh);
      if (code != 1 || RUN_SIM_US <= 0) begin
        RUN_SIM_US = 2000;
      end
    end

    $display("%t  TB     run length %0d us", $realtime, RUN_SIM_US);
  endtask

//--------------------------------------------------------------
// Clock and reset generation
//--------------------------------------------------------------

  // The 100 MHz PCIe reference clock, as supplied by the backplane. The link
  // clocks themselves are made inside the design (clk_rst_gen's MMCM) from the
  // TXOUTCLK that the PIPE PHY model provides.
  localparam HALF_REFCLK_PERIOD_PS = 5_000;   // 100 MHz
  localparam RST_CYCLES            = 200;

  logic refclk_p;
  wire  refclk_n;
  logic perst_n;

  initial begin
    get_run_length();
    refclk_p = 1'b0;
    perst_n = 1'b0;

    fork
      begin : board_rst_n
        #(HALF_REFCLK_PERIOD_PS * RST_CYCLES * 1ns / 1000.0)
          perst_n = 1'b1;
        $display("%t  TB     PERST# released", $realtime);
      end

      forever begin : refclk_gen
        #(HALF_REFCLK_PERIOD_PS * 1ns / 1000.0)
          refclk_p = ~refclk_p;
      end

      begin : run_sim
        fork
          begin : time_limit
            #(RUN_SIM_US * 1us);
            $display("\n%t  TB     *** simulation time limit reached ***", $realtime);
          end
          begin : result_tail
            @(result_seen);
            #(RESULT_TAIL_US * 1us);
            $display("\n%t  TB     *** run ends %0d us after the firmware result ***", $realtime,
                     RESULT_TAIL_US);
          end
        join_any
        report_status();  // ends with $fatal unless PASS, so make run fails
        $finish(2);
      end
    join
  end

  assign refclk_n = ~refclk_p;

//--------------------------------------------------------------
// DUT -- the real opensource Root Complex
//--------------------------------------------------------------

  wire [link_pkg::PCIE_LANES-1:0] rxp, rxn, txp, txn;  // unused: the link runs over PIPE
  wire [3:0] led_link_up;
  wire       clk_req;

  assign rxp = 1'b0;
  assign rxn = 1'b1;

  RC_opensource dut (
    .refclk_p    (refclk_p),
    .refclk_n    (refclk_n),

    .rxn          (rxn),
    .rxp          (rxp),
    .txn          (txn),
    .txp          (txp),

    .perst_n    (perst_n),

    .led_link_up  (led_link_up),
    .clk_req      (clk_req)
  );

//--------------------------------------------------------------
// Link-up watch
//--------------------------------------------------------------
// led_link_up[2] is driven with ~trn_link_up by the design, so it falling is
// the moment the PCIe link reaches L0 and the SOC comes out of reset.

  logic link_up_seen;

  initial link_up_seen = 1'b0;

  // Individual link status bits, so a stall can be pinned to a layer
  always @(dut.u_pcie.u_core.u_pcie_block.PLPHYLNKUPN)
    $display("%t  TB     pl_phy_lnk_up = %b", $realtime, !dut.u_pcie.u_core.u_pcie_block.PLPHYLNKUPN);
  always @(dut.core_rst_n)
    $display("%t  TB     core_rst_n   = %b", $realtime, dut.core_rst_n);
  always @(dut.trn_link_up)
    $display("%t  TB     trn_link_up   = %b", $realtime, dut.trn_link_up);

  always @(negedge led_link_up[2]) begin
    if (!link_up_seen) begin
      link_up_seen = 1'b1;
      $display("\n%t  TB     ===> PCIe LINK UP -- SOC released from reset\n", $realtime);
    end
  end

//--------------------------------------------------------------
// TLP watch on the SOC's AXI-Stream interface
//--------------------------------------------------------------
// This is the software's view of the link: every TLP the firmware pushes out
// and every completion that comes back. It is the quickest way to see whether
// enumeration is making progress without opening a waveform.

`ifndef NO_TLP_TRACE
  always @(posedge dut.user_clk) begin
    if (dut.s_axis_tx.tvalid && dut.s_axis_tx.tready && !dut.s_axis_tx.tlast) begin
      $display("%t  TX-TLP hdr0=%08h hdr1=%08h", $realtime,
               dut.s_axis_tx.tdata[31:0], dut.s_axis_tx.tdata[63:32]);
    end
    if (dut.m_axis_rx.tvalid && dut.m_axis_rx.tready) begin
      $display("%t  RX-TLP %016h", $realtime, dut.m_axis_rx.tdata);
    end
  end
`endif

// The same two streams, re-timed to one DW per clock with SOP/EOP flags, the
// shape a waveform viewer's PCIe TLP decoder takes (models/tlp_dw_monitor.sv):
//   tlp_tx  RC -> EP, what the firmware sends
//   tlp_rx  EP -> RC, the completions coming back

  tlp_dw_if tx_dw (dut.user_clk);
  tlp_dw_if rx_dw (dut.user_clk);

  tlp_dw_monitor tlp_tx (
    .axis_tdata  (dut.s_axis_tx.tdata),
    .axis_tkeep  (dut.s_axis_tx.tkeep),
    .axis_tlast  (dut.s_axis_tx.tlast),
    .axis_tvalid (dut.s_axis_tx.tvalid),
    .axis_tready (dut.s_axis_tx.tready),
    .dw          (tx_dw)
  );

  tlp_dw_monitor tlp_rx (
    .axis_tdata  (dut.m_axis_rx.tdata),
    .axis_tkeep  (dut.m_axis_rx.tkeep),
    .axis_tlast  (dut.m_axis_rx.tlast),
    .axis_tvalid (dut.m_axis_rx.tvalid),
    .axis_tready (dut.m_axis_rx.tready),
    .dw          (rx_dw)
  );

// Both streams again, as the nine-signal sets the WaveCrux decoder plugin
// auto-binds to: prefix tx_ decodes RC -> EP, rx_ decodes EP -> RC, each with
// the other direction as its peer (models/tlp_wavecrux_view.sv)

  tlp_wavecrux_view tlp_view (
    .tx (tx_dw),
    .rx (rx_dw)
  );

// The SOC CPU under RVFI names, for WaveCrux's RISC-V views
// (2.rtl/0.common.opensource/src/CPU.eduBOS5/wavecrux/soc_cpu_view.sv)
`ifdef DUMP_SOC_CPU
  soc_cpu_view cpu_view (
    .clk      (dut.u_soc.clk),
    .valid    (dut.u_soc.cpu.rvfi_valid),
    .order    (dut.u_soc.cpu.rvfi_order),
    .insn     (dut.u_soc.cpu.rvfi_insn),
    .trap     (dut.u_soc.cpu.rvfi_trap),
    .halt     (dut.u_soc.cpu.rvfi_halt),
    .intr     (dut.u_soc.cpu.rvfi_intr),
    .rs1_addr (dut.u_soc.cpu.rvfi_rs1_addr),
    .rs2_addr (dut.u_soc.cpu.rvfi_rs2_addr),
    .rs1_rdata(dut.u_soc.cpu.rvfi_rs1_rdata),
    .rs2_rdata(dut.u_soc.cpu.rvfi_rs2_rdata),
    .rd_addr  (dut.u_soc.cpu.rvfi_rd_addr),
    .rd_wdata (dut.u_soc.cpu.rvfi_rd_wdata),
    .pc_rdata (dut.u_soc.cpu.rvfi_pc_rdata),
    .pc_wdata (dut.u_soc.cpu.rvfi_pc_wdata),
    .mem_addr (dut.u_soc.cpu.rvfi_mem_addr),
    .mem_rmask(dut.u_soc.cpu.rvfi_mem_rmask),
    .mem_wmask(dut.u_soc.cpu.rvfi_mem_wmask),
    .mem_rdata(dut.u_soc.cpu.rvfi_mem_rdata),
    .mem_wdata(dut.u_soc.cpu.rvfi_mem_wdata)
  );
`endif

//--------------------------------------------------------------
// VCD dump, for viewers that do not read xsim's .wdb (e.g. WaveCrux):
//     make run SIM_GEN=1 USRSIMOPTS="--define DUMP_VCD"  -> tb.vcd, converted to tb.x1g1.direct.fst
// Add --define DUMP_SOC_CPU for the CPU under RVFI names (CPU.eduBOS5/wavecrux).
// Only the TLP monitors, the decoder view, the SOC level, the LTSSM
// state and the decoder page of the pcieVHost endpoint (u_pcie_ep, our
// wrapper openPCIE_BFM PIPExN__pcieVHost.sv): its 16-bit PIPE for Ferrite's PCIe
// PIPE and Data Link Layer decoders (plus rx_active for rxvalid), and the
// TLPs and DLLPs it sends and receives, from the taps we added to the model.
// That keeps the file small enough to open in seconds.
//--------------------------------------------------------------

`ifdef DUMP_VCD
  initial begin
    $dumpfile("tb.vcd");
    $dumpvars(1, tlp_tx);
    $dumpvars(1, tlp_rx);
    $dumpvars(1, tlp_view);
    $dumpvars(1, dut.u_soc);
    $dumpvars(0, dut.u_pcie.pl_ltssm_state_int);
    // the wrapper's level only: the model below it has shift registers that
    // would make the file four times larger
    $dumpvars(1, dut.u_pcie.u_phy_top.u_pcie_ep);
`ifdef PIPE_X4
    // make SIM_LANES=4: all four lanes, for the multi-lane PIPE and DLL decoders
    $dumpvars(1, dut.u_pcie.u_phy_top.u_x4);
`endif
    $dumpvars(0, dut.u_pcie.u_phy_top.rx_active);
`ifdef DUMP_SOC_CPU
    // the SOC CPU under RVFI names; last, so the ids of the signals above stay put
    $dumpvars(1, cpu_view);
`endif
  end
`endif

//--------------------------------------------------------------
// CPU activity
//--------------------------------------------------------------
// Confirms the CPU is actually executing, and how fast. Without it there
// is no way to tell a stalled CPU from one still inside a long delay loop.

`ifdef CPU_TRACE
  // What the firmware polls before it will send anything: send_tlp() spins on
  // status.phy until the Tx FSM is idle AND the hard macro reports at least one
  // free transmit buffer.
  always @(dut.cfg.tx_buf_avail)
    $display("%t  CSR    tx_buf_avail = %0d", $realtime, dut.cfg.tx_buf_avail);

  // Every CPU access into the CSR window
  integer csr_acc;
  initial csr_acc = 0;

  always @(posedge dut.user_clk) begin
    if ((dut.u_soc.u_soc_csr.cpuif_req && (|dut.u_soc.mem.wstrb)) ||
        (dut.u_soc.mem.valid && dut.u_soc.is_bridge && dut.u_soc.mem.ready && !(|dut.u_soc.mem.wstrb))) begin
      csr_acc = csr_acc + 1;
      if (csr_acc <= 300 || (csr_acc % 5000) == 0) begin
        $display("%t  CSR    %s [0x%02h] data=%08h  (#%0d)", $realtime,
                 (|dut.u_soc.mem.wstrb) ? "WR" : "RD",
                 dut.u_soc.mem.addr[7:0],
                 (|dut.u_soc.mem.wstrb) ? dut.u_soc.mem.wdata
                                           : dut.u_soc.mem.rdata, csr_acc);
      end
    end
  end

  // First bus accesses after reset release, to see where the CPU actually goes.
  // make PLUSARGS=BUS_TRACE_N=<n> for another count (default 60).
  integer first_n;
  int     bus_trace_n;
  initial begin
    first_n = 0;
    if (!$value$plusargs("BUS_TRACE_N=%d", bus_trace_n)) bus_trace_n = 60;
  end

  always @(posedge dut.user_clk) begin
    if (dut.u_soc.mem.valid && dut.u_soc.mem.ready && first_n < bus_trace_n) begin
      first_n = first_n + 1;
      $display("%t  BUS  %2d  addr=%08h %s data=%08h", $realtime, first_n,
               dut.u_soc.mem.addr,
               (|dut.u_soc.mem.wstrb) ? "WR" : "RD",
               (|dut.u_soc.mem.wstrb) ? dut.u_soc.mem.wdata
                                         : dut.u_soc.mem.rdata);
    end
  end

  integer cpu_acc;
  integer cpu_pc_min, cpu_pc_max;

  initial begin
    cpu_acc = 0; cpu_pc_min = 32'h7fffffff; cpu_pc_max = 0;
  end

  always @(posedge dut.user_clk) begin
    if (dut.u_soc.mem.valid && dut.u_soc.mem.ready) begin
      cpu_acc = cpu_acc + 1;
      if (dut.u_soc.is_ram) begin
        if (dut.u_soc.mem.addr < cpu_pc_min) cpu_pc_min = dut.u_soc.mem.addr;
        if (dut.u_soc.mem.addr > cpu_pc_max) cpu_pc_max = dut.u_soc.mem.addr;
      end
    end
  end

  initial forever begin
    #(1000 * 1us);
    $display("%t  CPU    %0d bus accesses, RAM addr range 0x%08h..0x%08h",
             $realtime, cpu_acc, cpu_pc_min, cpu_pc_max);
  end
`endif

//--------------------------------------------------------------
// Firmware result
//--------------------------------------------------------------
// main() ends by writing a marker into status.result:
//   0x0000FACE  memory write/read back matched
//   0x0000DEAD  it did not, or the read was not completed with SC
//   0xBAD00000  no device answered the first config read (RC-switched:
//               the switch upstream port)
//   0xBAD00001  RC-switched only: the switch is up, no endpoint behind it
//   0xBAD00002  a later config request failed (UR/CA/timeout)
//   0xBAD00003  BAR0 is not implemented, is I/O, or does not fit
//   0xBAD00004  the burst self-test (PIO_BURST=1) read back other data
//   0xBAD00005  the Tx side never came free (wait_tx_ready)
// Everything before that is the enumeration sequence.

  logic [31:0] result_marker;
  integer      tlp_tx_count;

  // The marker only says pass or fail. These carry the payload the test
  // actually moved across the link, so the summary can show the data rather
  // than just the verdict. They watch the SOC's TLP streams, not its CSR:
  //   mem_wr_payload  the dword of the last 1-DW Memory Write TLP
  //   mem_rd_payload  the dword of the last 1-DW CplD, frozen when the
  //                   marker lands: the memory read-back
  //   burst_wr_dw     payload DWORDs of Memory Writes longer than 1 DW
  //   burst_rd_dw     payload DWORDs of CplDs longer than 1 DW
  logic [31:0] mem_wr_payload;
  logic [31:0] mem_rd_payload;
  logic [31:0] rx_data_last;
  logic        mem_wr_seen;
  integer      burst_wr_dw;
  integer      burst_rd_dw;

  initial begin
    result_marker  = 32'hffff_ffff;
    tlp_tx_count   = 0;
    mem_wr_payload = 32'h0;
    mem_rd_payload = 32'h0;
    rx_data_last   = 32'h0;
    mem_wr_seen    = 1'b0;
    burst_wr_dw    = 0;
    burst_rd_dw    = 0;
  end

  // A TLP starts with DW0 in tdata[31:0] and DW1 in [63:32]; DW2 and the
  // first payload DWORD of a 3-DW header follow in the next beat
  bit          tx_sop = 1'b1, tx_beat1 = 1'b0, rx_sop = 1'b1, rx_beat1 = 1'b0;
  logic [31:0] tx_dw0, rx_dw0;
  integer      tx_beat_n = 0;  // beat of the TLP on s_axis_tx, 0 = DW0/DW1

  // The throughput report (PIO_BURST=1, 3.sw/common/pcie.c pcie_perf_test):
  // a 7-DW Memory Write to BAR0 + 0x300: magic | DWORDs moved, the cycles of
  // 1-DW writes, burst write, 1-DW reads, burst read, the CPU clock in kHz,
  // and the 1-DW writes as one batch
  logic [31:0] perf_rep[7];
  bit          perf_tlp = 1'b0, perf_seen = 1'b0;

  always @(posedge dut.user_clk) begin
    if (dut.s_axis_tx.tvalid && dut.s_axis_tx.tready) begin
      if (tx_sop) begin
        tx_dw0       = dut.s_axis_tx.tdata[31:0];
        tlp_tx_count = tlp_tx_count + 1;
        tx_beat_n    = 0;
        // Fmt/Type 0x40: a 3-DW Memory Write
        if (tx_dw0[31:24] == 8'h40 && tx_dw0[9:0] > 10'd1) begin
          burst_wr_dw = burst_wr_dw + tx_dw0[9:0];
        end
      end
      else begin
        tx_beat_n = tx_beat_n + 1;
      end
      if (tx_beat1 && tx_dw0[31:24] == 8'h40 && tx_dw0[9:0] == 10'd1) begin
        mem_wr_payload = dut.s_axis_tx.tdata[63:32];
        mem_wr_seen    = 1'b1;
      end
      // beat k carries DW 2k and 2k+1; payload DWORD i is DW 3+i
      if (tx_beat1) begin
        perf_tlp = (tx_dw0[31:24] == 8'h40) && (tx_dw0[9:0] == 10'd7) &&
                   (dut.s_axis_tx.tdata[11:0] == 12'h300);
      end
      if (perf_tlp && tx_beat_n >= 1) begin
        if (2 * tx_beat_n - 3 >= 0) perf_rep[2*tx_beat_n-3] = dut.s_axis_tx.tdata[31:0];
        if (2 * tx_beat_n - 2 < 7) perf_rep[2*tx_beat_n-2] = dut.s_axis_tx.tdata[63:32];
        if (dut.s_axis_tx.tlast) perf_seen = (perf_rep[0][31:16] == 16'h9E5F);
      end
      tx_beat1 = tx_sop & ~dut.s_axis_tx.tlast;
      tx_sop   = dut.s_axis_tx.tlast;
      if (dut.s_axis_tx.tlast) perf_tlp = 1'b0;
    end
  end

  // MB/s of n DWORDs moved in c cycles at f kHz
  function automatic real perf_mbs(input integer n, input logic [31:0] c, input logic [31:0] f);
    return (c == 0) ? 0.0 : real'(4 * n) * real'(f) / (real'(c) * 1000.0);
  endfunction

  // The perf check, on the reference run only (the 3.sw README table; the
  // make file writes its ceilings into perf_max.cfg, 0 elsewhere): no count
  // above its ceiling. Ceilings only, no ratios: a faster 1-DW path lowers
  // every gain, and must not fail the check. Other builds only print
  bit perf_fail;  // a bit starts at 0

  task automatic perf_max_check(input string what, input logic [31:0] c, input integer max);
    if (max > 0 && c > max) begin
      $display("  PERF CHECK .............. FAIL: %s %0d cycles, ceiling %0d", what, c, max);
      perf_fail = 1'b1;
    end
  endtask

  task automatic check_perf();
    integer fh, code;
    integer mx[5];  // 1-DW writes, batch, burst write, 1-DW reads, burst read

    mx = '{default: 0};
    fh = $fopen("perf_max.cfg", "r");
    if (fh != 0) begin
      code = $fscanf(fh, "%d %d %d %d %d", mx[0], mx[1], mx[2], mx[3], mx[4]);
      $fclose(fh);
    end
    if (mx[0] <= 0) begin
      $display("  PERF CHECK .............. not this build (reference run only)");
      return;
    end
    perf_max_check("1-DW writes", perf_rep[1], mx[0]);
    perf_max_check("batch", perf_rep[6], mx[1]);
    perf_max_check("burst write", perf_rep[2], mx[2]);
    perf_max_check("1-DW reads", perf_rep[3], mx[3]);
    perf_max_check("burst read", perf_rep[4], mx[4]);
    if (!perf_fail) begin
      $display("  PERF CHECK .............. PASS");
    end
  endtask

  task automatic report_perf();
    integer n;
    n = perf_rep[0][15:0];
    if (perf_rep[5] == 0) begin
      $display("  PIO throughput .......... not measured (no cycle counter)");
      return;
    end
    $display("  PIO throughput, %0d DWORDs (%0d bytes), firmware cycles at %0.1f MHz:",
             n, 4 * n, real'(perf_rep[5]) / 1000.0);
    $display("    write  %2d x 1-DW  %6d cycles  %6.1f MB/s", n, perf_rep[1],
             perf_mbs(n, perf_rep[1], perf_rep[5]));
    $display("    write  %2d x 1-DW  %6d cycles  %6.1f MB/s  %0.1fx  (one batch)", n,
             perf_rep[6], perf_mbs(n, perf_rep[6], perf_rep[5]),
             real'(perf_rep[1]) / real'(perf_rep[6]));
    $display("    write   1 x %0d-DW %6d cycles  %6.1f MB/s  %0.1fx", n, perf_rep[2],
             perf_mbs(n, perf_rep[2], perf_rep[5]), real'(perf_rep[1]) / real'(perf_rep[2]));
    $display("    read   %2d x 1-DW  %6d cycles  %6.1f MB/s", n, perf_rep[3],
             perf_mbs(n, perf_rep[3], perf_rep[5]));
    $display("    read    1 x %0d-DW %6d cycles  %6.1f MB/s  %0.1fx", n, perf_rep[4],
             perf_mbs(n, perf_rep[4], perf_rep[5]), real'(perf_rep[3]) / real'(perf_rep[4]));
    check_perf();
  endtask

  always @(posedge dut.user_clk) begin
    if (dut.m_axis_rx.tvalid && dut.m_axis_rx.tready) begin
      if (rx_sop) begin
        rx_dw0 = dut.m_axis_rx.tdata[31:0];
        // Fmt/Type 0x4A: CplD
        if (rx_dw0[31:24] == 8'h4A && rx_dw0[9:0] > 10'd1) begin
          burst_rd_dw = burst_rd_dw + rx_dw0[9:0];
        end
      end
      if (rx_beat1 && rx_dw0[31:24] == 8'h4A && rx_dw0[9:0] == 10'd1) begin
        rx_data_last = dut.m_axis_rx.tdata[63:32];
      end
      rx_beat1 = rx_sop & ~dut.m_axis_rx.tlast;
      rx_sop   = dut.m_axis_rx.tlast;
    end
  end

  // The marker is a CSR write, counted on the request strobe, once per
  // access: picorv32 holds valid until ready, but eduBOS5 stores are
  // one-cycle requests whose ready comes after valid has dropped.
  // status.result is at offset 0x2C (csr.rdl); the whole offset is compared,
  // as the payload windows (0x1000, 0x2000) repeat its low byte
  always @(posedge dut.user_clk) begin
    if (dut.u_soc.u_soc_csr.cpuif_req && (|dut.u_soc.mem.wstrb) &&
        dut.u_soc.mem.addr[13:0] == 14'h002c) begin
      result_marker  = dut.u_soc.mem.wdata;
      mem_rd_payload = rx_data_last;
      $display("\n%t  TB     ===> FIRMWARE RESULT 0x%08h  (%s)\n", $realtime,
               dut.u_soc.mem.wdata,
               (dut.u_soc.mem.wdata == 32'h0000_face) ? "PASS" : "FAIL");
      ->result_seen;
    end
  end

`ifdef SOC_IPC
  // IPC: instructions the CPU retires (RVFI) per user_clk cycle, from SOC
  // reset release to the firmware result. The Makefile adds RISCV_FORMAL.
  longint ipc_cycles = 0, ipc_instrs = 0;
  bit     ipc_on = 1'b1;
  always @(posedge dut.user_clk) begin
    if (ipc_on && dut.u_soc.resetn) begin
      ipc_cycles++;
      if (dut.u_soc.cpu.rvfi_valid) ipc_instrs++;
    end
  end
  always @(result_seen) begin
    ipc_on = 1'b0;
    $display("%t  TB     IPC %0d instructions / %0d cycles = %0.4f", $realtime, ipc_instrs,
             ipc_cycles, real'(ipc_instrs) / real'(ipc_cycles));
  end
`endif

//--------------------------------------------------------------
// Final report
//--------------------------------------------------------------

  task automatic report_status();
    $display("--------------------------------------------------------");
    $display(" openpcie2-rc co-simulation summary");
    $display("--------------------------------------------------------");
    $display("  PCIe link up ............ %s", link_up_seen ? "YES" : "NO");
    $display("  LTSSM final state ....... 0x%02h",
             dut.u_pcie.pl_ltssm_state_int);
    $display("  TLPs sent by firmware ... %0d", tlp_tx_count);

    if (mem_wr_seen) begin
      $display("  Payload written to EP ... 0x%08h  (Memory Write TLP)",
               mem_wr_payload);
      // if/else rather than a ternary: Verilog widens both string literals to
      // the longer one, which would left-pad the shorter with NULs
      if (mem_rd_payload === mem_wr_payload)
        $display("  Payload read back ....... 0x%08h  (match)", mem_rd_payload);
      else
        $display("  Payload read back ....... 0x%08h  (MISMATCH)", mem_rd_payload);
    end
    if (burst_wr_dw != 0 || burst_rd_dw != 0) begin
      $display("  Burst DWORDs ............ %0d written, %0d completed", burst_wr_dw,
               burst_rd_dw);
    end
    if (perf_seen) report_perf();

    case (result_marker)
      32'h0000_face: $display("  FIRMWARE RESULT ......... PASS (0x%08h)", result_marker);
      32'h0000_dead: $display("  FIRMWARE RESULT ......... FAIL (0x%08h)", result_marker);
      32'hbad0_0000: $display("  FIRMWARE RESULT ......... no device at bus 1 (switched: the switch) (0x%08h)", result_marker);
      32'hbad0_0001: $display("  FIRMWARE RESULT ......... switch up, no endpoint behind it (0x%08h)", result_marker);
      32'hbad0_0002: $display("  FIRMWARE RESULT ......... config request failed (0x%08h)", result_marker);
      32'hbad0_0003: $display("  FIRMWARE RESULT ......... BAR0 not usable (0x%08h)", result_marker);
      32'hbad0_0004: $display("  FIRMWARE RESULT ......... burst read back wrong (0x%08h)", result_marker);
      32'hbad0_0005: $display("  FIRMWARE RESULT ......... Tx side stuck (0x%08h)", result_marker);
      default:       $display("  FIRMWARE RESULT ......... not reached yet");
    endcase
    $display("--------------------------------------------------------");
    // the result sets the exit status
    if (result_marker !== 32'h0000_face) begin
      $fatal(1, "firmware result 0x%08h, not PASS (0x0000face)", result_marker);
    end
    if (perf_fail) begin
      $fatal(1, "PIO throughput below its check (PERF CHECK above)");
    end
  endtask

// Top level fatal task, which can be called from anywhere in verilog code
// via the `fatal definition in pciedispheader.svh.
task Fatal;
begin
    $fatal(1, "pcieVHost fatal: a model's FAIL, or a bad VProc access");
end
endtask

endmodule: tb

/*
------------------------------------------------------------------------------
Version History:
------------------------------------------------------------------------------
 2025/08/19 SS: initial creation, against the dut_stub stand-in
 2026/08/11:    retargeted at the real RC_direct_opensource RTL, with the
                pcievhost endpoint moved behind the PIPE PHY model
 2026/10/07:    DUT is RC_opensource from 2.rtl/0.common.opensource, the RTL
                shared by RC-direct and RC-switched; 0xBAD00001 decoded
 2026/10/08:    DUMP_VCD also dumps the 16-bit PIPE at the pcieVHost endpoint,
                for Ferrite's PCIe PIPE / Data Link Layer WaveCrux decoders
 2026/10/17:    firmware result read from status.result, which sends no TLP
*/

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
