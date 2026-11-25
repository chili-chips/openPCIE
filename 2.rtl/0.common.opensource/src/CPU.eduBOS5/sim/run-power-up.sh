#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# eduBOS5 before reset acts, in a 4-state and a 2-state simulator: Icarus
# Verilog (after sv2v, as Icarus does not read `unique if`) and Verilator.
# Both pipelines, and the 3-stage with RF_SYNC_READ; `define RISCV_FORMAL
# (RVFI outputs on).
#
# usage: bash run-power-up.sh
# Needs iverilog/vvp, sv2v and verilator on PATH.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
C=$HERE/..
SRC=("$C"/edubos5_pkg.sv "$C"/edubos5_rf.sv "$C"/edubos5_alu.sv "$C"/edubos5.sv "$HERE"/tb_power_up.sv)
W=$(mktemp -d /tmp/power_up_edubos5.XXXX)
cd "$W"

sv2v -DRISCV_FORMAL "${SRC[@]}" > all.v
# no `timescale in the sources: the simulator command line gives it
echo "+timescale+1ns/10ps" > timescale.cf
for cfg in 0:0 1:0 1:1; do  # EN_WAIT4REGS:RF_SYNC_READ
  en=${cfg%:*}
  rs=${cfg#*:}
  echo "== Icarus, EN_WAIT4REGS=$en RF_SYNC_READ=$rs"
  iverilog -g2012 -c timescale.cf -P tb_power_up.EN_WAIT4REGS=$en -P tb_power_up.RF_SYNC_READ=$rs \
    -o ivl_$en$rs.vvp all.v
  vvp -n ivl_$en$rs.vvp | grep '^t='
done

for cfg in 0:0 1:0 1:1; do
  en=${cfg%:*}
  rs=${cfg#*:}
  echo "== Verilator, EN_WAIT4REGS=$en RF_SYNC_READ=$rs"
  verilator --binary --timing --timescale 1ns/10ps -Wno-fatal -Wno-lint -Wno-style -DRISCV_FORMAL \
    -GEN_WAIT4REGS=$en -GRF_SYNC_READ=$rs --top-module tb_power_up -Mdir vl_$en$rs "${SRC[@]}" \
    > vl_$en$rs.log 2>&1
  ./vl_$en$rs/Vtb_power_up | grep '^t='
done
echo "work folder: $W"
