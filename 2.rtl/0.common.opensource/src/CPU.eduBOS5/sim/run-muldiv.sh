#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# RV32M test of eduBOS5 (USE_HW_MUL=1, tb_muldiv.sv): all 8 M instructions on
# corner and random operands against a reference, and a fixed count of clocks
# per instruction. Both pipelines, and the 3-stage with RF_SYNC_READ, each
# with STORE_WAITS_RDY 0 and 1, in
# Verilator, the multiplier in RTL (USE_DSP48E1=0). Icarus (after sv2v) runs
# fewer random vectors, with the RTL multiplier and, given AMD's simulation
# model, with the DSP48E1 primitive (USE_DSP48E1=1).
#
# usage: [N_RAND=<n>] [IVL_RAND=<n>] [UNISIMS=<Vivado>/data/verilog/src] bash run-muldiv.sh
#   UNISIMS  holds unisims/DSP48E1.v and glbl.v; default: from XILINX_VIVADO
# Needs verilator, and iverilog/vvp with sv2v for the Icarus runs.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
C=$HERE/..
SRC=("$C"/edubos5_pkg.sv "$C"/edubos5_rf.sv "$C"/edubos5_alu.sv "$C"/edubos5.sv "$HERE"/tb_muldiv.sv)
N_RAND=${N_RAND:-20000}
IVL_RAND=${IVL_RAND:-500}
UNISIMS=${UNISIMS:-${XILINX_VIVADO:+$XILINX_VIVADO/data/verilog/src}}
W=$(mktemp -d /tmp/muldiv_edubos5.XXXX)
cd "$W"

for cfg in 0:0 1:0 1:1; do  # EN_WAIT4REGS:RF_SYNC_READ
  en=${cfg%:*}
  rs=${cfg#*:}
  for sw in 0 1; do
    echo "== Verilator, EN_WAIT4REGS=$en RF_SYNC_READ=$rs STORE_WAIT=$sw"
    verilator --binary --timing --timescale 1ns/10ps -Wno-fatal -Wno-lint -Wno-style -DRISCV_FORMAL \
      -GEN_WAIT4REGS=$en -GRF_SYNC_READ=$rs -GSTORE_WAIT=$sw -GN_RAND="$N_RAND" \
      --top-module tb_muldiv -Mdir vl_$en${rs}_$sw "${SRC[@]}" > vl_$en${rs}_$sw.log 2>&1
    ./vl_$en${rs}_$sw/Vtb_muldiv | grep -E '^(funct3|reuse|PASS|FAIL)'
  done
done

if command -v iverilog > /dev/null && command -v sv2v > /dev/null; then
  sv2v -DRISCV_FORMAL "${SRC[@]}" > all.v
  echo "+timescale+1ns/1ps" > timescale.cf
  DSPS=0
  [ -n "$UNISIMS" ] && [ -f "$UNISIMS/unisims/DSP48E1.v" ] && DSPS="0 1"
  for dsp in $DSPS; do
    for cfg in 0:0 1:0 1:1; do  # EN_WAIT4REGS:RF_SYNC_READ
      en=${cfg%:*}
      rs=${cfg#*:}
      echo "== Icarus, EN_WAIT4REGS=$en RF_SYNC_READ=$rs USE_DSP48E1=$dsp"
      lib=()
      [ "$dsp" = 1 ] && lib=(-s glbl "$UNISIMS/unisims/DSP48E1.v" "$UNISIMS/glbl.v")
      iverilog -g2012 -c timescale.cf -P tb_muldiv.EN_WAIT4REGS=$en -P tb_muldiv.RF_SYNC_READ=$rs \
        -P tb_muldiv.N_RAND="$IVL_RAND" -P tb_muldiv.USE_DSP48E1=$dsp -s tb_muldiv \
        -o ivl_$en${rs}_$dsp.vvp all.v "${lib[@]}" > ivl_$en${rs}_$dsp.log 2>&1
      vvp -n ivl_$en${rs}_$dsp.vvp | grep -E '^(funct3|reuse|PASS|FAIL)'
    done
  done
fi
echo "work folder: $W"
