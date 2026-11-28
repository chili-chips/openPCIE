#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
# SPDX-License-Identifier: BSD-3-Clause
#
# tb_soc on Verilator: the SOC alone (soc_top), eduBOS5 running the firmware
# of one test in 3.sw/tests/, against a stand-in for the PCIE stack. From
# openCologne-PCIE's 2.rtl/7.soc/sim/run-soc.sh.
# usage:  bash run-soc.sh tx    3.sw/tests/soc_tx: 3-DW and 4-DW requests,
#                               single and batched; every s_axis_tx beat checked
#         bash run-soc.sh rx    3.sw/tests/soc_rx: 96 TLPs, more than the RX
#                               ring holds, sent back as they came
#         bash run-soc.sh cpl   3.sw/tests/soc_cpl: a split burst read behind
#                               a Message and a stale Completion; a UR
#         bash run-soc.sh all   the three
# Prints the test's verdict line ("TEST PASSED"); exits 1 if any failed.
# SOC_BUILD (default ~/.cache/openpcie/soc) is the build directory; the
# waveform is $SOC_BUILD/<test>/tb_soc.fst.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
S=$R/2.rtl/0.common.opensource/src
P=$S/pcie
E=$S/CPU.eduBOS5
G=$R/4.build/csr_build/generated-files
B=${SOC_BUILD:-$HOME/.cache/openpcie/soc}
mkdir -p "$B"

case "${1:-}" in
  tx | rx | cpl) TESTS=("$1") ;;
  all) TESTS=(tx rx cpl) ;;
  *) sed -n '8,15p' "$0"; exit 2 ;;
esac

# SOC_MUL_RTL: eduBOS5 multiplies in RTL, as Verilator has no DSP48E1 model
verilator --binary --timing --trace-fst -Wno-fatal -Wno-lint -Wno-style --timescale 1ns/10ps \
  --assert -DSOC_MUL_RTL --top-module tb_soc --Mdir "$B/obj" -o Vtb_soc \
  "$P/link_pkg.sv" "$P/pcie_pkg.sv" "$G/csr_pkg.sv" "$E/edubos5_pkg.sv" \
  "$P/axis_if.sv" "$P/cfg_if.sv" "$P/mem_if.sv" \
  "$E/edubos5_rf.sv" "$E/edubos5_alu.sv" "$E/edubos5.sv" "$G/csr.sv" \
  "$S/soc_csr.sv" "$S/soc_top.sv" "$HERE/tb_soc.sv" > "$B/build.log" 2>&1 ||
  { tail -20 "$B/build.log"; exit 1; }

fails=0
for t in "${TESTS[@]}"; do
  T=soc_$t
  mkdir -p "$B/$T"
  make -s -C "$R/4.build/sw_build" SW_DIR=../../3.sw/tests/$T OUT="$B/$T" > "$B/$T.log" 2>&1 ||
    { tail -20 "$B/$T.log"; exit 1; }
  (cd "$B/$T" && "$B/obj/Vtb_soc" +${t}_check > run.log 2>&1) || true
  v=$(grep -a "TEST \(PASSED\|FAILED\)" "$B/$T/run.log" || echo "tb_soc: $t: no verdict, see $B/$T/run.log")
  echo "$v"
  case "$v" in *PASSED*) ;; *) fails=$((fails + 1)); grep -a "ERROR" "$B/$T/run.log" | head -5 ;; esac
done
[ $fails = 0 ]

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        Nov 27, 2026
# -----------------------------------------------------------------------------
