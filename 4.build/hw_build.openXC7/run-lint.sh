#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Lint of the opensource RTL (2.rtl/0.common.opensource/src): syntax and
# limited semantic checks with a set of opensource tools, run in this order:
#
#   tags       every /*LINT:OK*/ sits on an output port, and every unique and
#              priority case of our RTL has a default (no tool needed)
#   verilator  Verilator --lint-only -Wall: unused/undriven signals, widths,
#              implicit nets, multiple drivers, blocking vs non-blocking, case
#   yosys      Yosys check on the read_slang netlist: combinational loops,
#              conflicting drivers, used-but-undriven wires, latches
#   reset      Yosys netlist and run-lint-reset.py: a control flop or flag with
#              no reset (data and address flops need none: waived by name);
#              the top, then eduBOS5 alone with RV32M and the counters
#   ram        Yosys memory_libmap (synth_xilinx's) and run-lint-ram.py: a
#              memory mapped to block or LUT RAM with read-first or
#              transparency logic built in fabric
#   iob        the reset phase's netlist and run-lint-iob.py: every data pin of
#              the top reaches its flop directly (input: pin to D; output: Q to
#              pin and nothing else), so the flop can sit in the I/O tile; the
#              GTP and clock pins are skipped, the slow pins (LEDs, PERST#)
#              waived by name
#   slang      read_slang -Wextra: strict IEEE parse, second opinion (read_slang
#              has no --lint-only, that needs standalone slang)
#   verible    Verible style rules (run-lint.verible): our case rule -- signals
#              and ports lower case, constants UPPER case -- and Verible's defaults
#   cdc        Vivado report_cdc: clock-domain crossings and their
#              synchronizers (there is no mature opensource CDC checker)
#   check-timing
#              Vivado check_timing on the same synthesis: clocks and
#              constraints (a register with no clock, unconstrained
#              endpoints, ports with no I/O delay, loops); HIGH and MEDIUM
#   methodology
#              Vivado report_methodology on the same synthesis: the XDC and
#              timing rules (TIMING-*, XDC*, ...); Critical Warning and Warning;
#              plus every Vivado CRITICAL WARNING on an XDC line (a constraint
#              that matches nothing)
#
#              cdc, check-timing and methodology share one Vivado run
#              (about 3 min); skipped without Vivado, so not in CI. The ghdl
#              phases of wireguard-fpga lint VHDL: there is none here.
#
# usage:  bash run-lint.sh [phase ...]      default: all phases
#         LANES=1 bash run-lint.sh            lint the x1 link (default x4)
#         GEN=2 bash run-lint.sh              lint the Gen2 build (default: link_pkg.sv's,
#                                             Gen1 at x4, Gen2 at x1)
#         CPU=picorv32 bash run-lint.sh       lint the PicoRV32 build (default edubos5;
#                                             edubos5_3stg: 3-stage pipeline)
#
# A phase whose tool is not found is skipped. Source ../set-env.sh first; the
# cdc, check-timing and methodology phases need Vivado. The tool commands can be overridden: VERILATOR, YOSYS
# (+ YOSYS_ARGS), YOSYS_CONFIG, PYTHON, VERIBLE, VIVADO. The tags and Verilator phases
# need the Xilinx primitive stubs that gen_xilinx_bb.py builds from the Yosys
# library: from yosys-config, or pass XILINX_BB=<stubs.sv> from a run that had
# Yosys. read_slang is built into the openXC7 Yosys, a plugin in the OSS CAD
# Suite: when YOSYS_ARGS is not set, it is "" if the Yosys has read_slang,
# else "-m slang".
#
# Sources are copied into a work directory (LINT_WORK, default mktemp -d) and
# every tool gets relative paths, since Windows tools cannot read UNC or WSL
# paths. Logs stay there: <phase>.log is the tool output, <phase>.findings the
# findings left after the waivers.
#
# Accepted findings are waived in two ways, each one reviewed:
#   - inline, on an unused output of an instance:  .PIN (/*LINT:OK*/)
#     waives the findings about that pin, from any phase; the tags phase flags
#     a /*LINT:OK*/ on an input, so it cannot hide a floating input
#   - run-lint-waivers.txt (or LINT_WAIVERS=<file>), for findings without a
#     source line of ours (generated code, Vivado reports, ...)
# Exit status is non-zero when any phase has findings that are not waived, or
# when its tool fails or stops before the step it reports on (FAILED in the
# summary): "0 finding(s)" always means the phase really ran.
#
# TODO: Verilator lints the SYN_YOSYS_BUG variant of the RTL, as the stubs have
#       no real parameters.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../2.rtl/0.common.opensource
CSR=$HERE/../csr_build/generated-files
TOP=RC_opensource
PART=xc7a200tfbg484-3

VERILATOR=${VERILATOR:-verilator}
YOSYS=${YOSYS:-yosys}
YOSYS_CONFIG=${YOSYS_CONFIG:-yosys-config}
PYTHON=${PYTHON:-python3}
if [ -z "${YOSYS_ARGS+set}" ]; then   # "-m slang" where read_slang is a plugin
  if "$YOSYS" -p 'help read_slang' 2>/dev/null | grep -q 'read_slang \['; then
    YOSYS_ARGS=""
  else
    YOSYS_ARGS="-m slang"
  fi
fi
VERIBLE=${VERIBLE:-verible-verilog-lint}
VIVADO=${VIVADO:-vivado}
LANES=${LANES:-4}
GEN=${GEN:-}
CPU=${CPU:-edubos5}
PICORV32_SV="src/CPU.eduBOS5/edubos5_pkg.sv src/picorv32.CHILI.sv src/picorv32_pcpi_mul.sv src/picorv32_pcpi_div.sv"  # soc_top imports edubos5_pkg
EDUBOS5_SV=$(for f in pkg rf alu; do echo src/CPU.eduBOS5/edubos5_$f.sv; done; echo src/CPU.eduBOS5/edubos5.sv)
case $CPU in
  picorv32) CPU_SV=$PICORV32_SV; OTHER_CPU_SV=$EDUBOS5_SV; CPU_DEF=SOC_CPU_PICORV32 ;;
  edubos5)  CPU_SV=$EDUBOS5_SV; OTHER_CPU_SV=$PICORV32_SV; CPU_DEF="" ;;
  edubos5_3stg) CPU_SV=$EDUBOS5_SV; OTHER_CPU_SV=$PICORV32_SV; CPU_DEF=SOC_EDUBOS5_3STG ;;
  *) echo "CPU must be picorv32, edubos5 or edubos5_3stg, got \"$CPU\""; exit 1 ;;
esac
VL_DEFS=""; SL_DEFS=""; VIV_DEFS=""
for d in $CPU_DEF; do
  VL_DEFS="$VL_DEFS -D$d"; SL_DEFS="$SL_DEFS -D $d"; VIV_DEFS="$VIV_DEFS -verilog_define $d"
done
WAIVERS=${LINT_WAIVERS:-$HERE/run-lint-waivers.txt}

PHASES=${*:-tags verilator yosys reset ram iob slang verible cdc check-timing methodology}
W=${LINT_WORK:-$(mktemp -d)}
mkdir -p "$W"
# a reused LINT_WORK keeps no stale sources, stubs or netlists (iob reads
# reset's netlist when both run)
rm -rf "$W/src" "$W/xdc" "$W/bb.sv" "$W"/reset*.json "$W"/ram*.json
PHASE_NAMES="tags verilator yosys reset ram iob slang verible cdc check-timing methodology"
for ph in $PHASES; do  # every name first, before minutes of Vivado
  case " $PHASE_NAMES " in *" $ph "*) ;; *) echo "unknown phase: $ph ($PHASE_NAMES)"; exit 2 ;; esac
done

# ---- work copy, in the same order as SV_ORDERED in the Makefile ---------------
cp -r "$SRC/src" "$SRC/xdc" "$W/"
cp "$CSR/csr_pkg.sv" "$CSR/csr.sv" "$HERE/unimacro_shim.v" "$W/"
cp "$HERE/../sw_build/firmware.hex" "$W/"
LANE_XDC=$([ "$LANES" = 4 ] && echo x4g1g2 || echo x1g2)   # one x4 file for Gen1 and Gen2
cp "$SRC/../2.RC-direct.opensource/xdc/RC-direct.$LANE_XDC.AcornCLE-215P.xdc" "$W/xdc/"
cd "$W"

P=src/pcie
SV_IF=$(for i in axis_if trn_if cfg_if mem_if mim_if gtp_if pipe_if; do echo $P/$i.sv; done)
SV_REST=$(ls $P/*.sv | grep -v -e '_if\.sv$' -e '_pkg\.sv$')
SV=$(echo $P/link_pkg.sv $P/pcie_pkg.sv csr_pkg.sv $SV_IF $SV_REST csr.sv src/soc_csr.sv \
          $CPU_SV src/soc_top.sv src/$TOP.sv)
SHIMS="unimacro_shim.v"
PRIMS="PCIE_2_1 GTPE2_CHANNEL GTPE2_COMMON MMCME2_ADV IBUFDS_GTE2 BUFG BUFGCTRL FDCE RAMB36E1 DSP48E1"

have() { command -v "$1" >/dev/null 2>&1; }
declare -A RESULT
skip() { RESULT[$1]="skipped ($2)"; echo "-- $1: skipped, $2"; }

# waiver regexes of one phase: the lines "<phase> <regex>" of the waivers file;
# a regex that never matches keeps grep -v -f from dropping every line when
# the phase has no waiver
waivers() {
  echo '^$'
  [ -f "$WAIVERS" ] || return 0
  awk -v ph="$1" '{ sub(/\r$/, "") } $1 == ph { sub(/^[ \t]*[^ \t]+[ \t]+/, ""); print }' "$WAIVERS"
}

# inline waivers .PIN (/*LINT:OK*/), as file:line:PIN (the first line keeps
# awk's NR==FNR test working when there is none)
{ echo "#"
  grep -n -o -E '\.[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\([[:space:]]*/\*LINT:OK\*/[[:space:]]*\)' $SV |
    sed -E 's/^([^:]*:[0-9]+):\.([A-Za-z0-9_]*).*/\1:\2/'
} > lint_ok.txt

found() {  # $1 phase; one finding per line on stdin (not a pipe: RESULT must stay in this shell)
  awk '!seen[$0]++' > $1.all  # a finding that several tops or runs share, once
  # drop findings about a pin waived inline: file:line plus the 'PIN' the tools
  # quote (Windows tools print \ in paths)
  awk 'NR == FNR { ok[$0] = 1; next }
       { l = $0; gsub(/\\/, "/", l)
         if (match(l, /[A-Za-z0-9_.\/-]+\.s?v:[0-9]+/)) {
           k = substr(l, RSTART, RLENGTH)
           if (match(l, /\047[A-Za-z_][A-Za-z0-9_]*\047/) && (k ":" substr(l, RSTART + 1, RLENGTH - 2)) in ok) next
         }
         print }' lint_ok.txt $1.all > $1.inline
  grep -E -v -f <(waivers $1) $1.inline > $1.findings
  local n=$(wc -l < $1.findings) t=$(wc -l < $1.all) i=$(wc -l < $1.inline)
  RESULT[$1]="$n finding(s), $((t - n)) waived ($((t - i)) inline), see $W/$1.findings"
  echo "-- $1: $n finding(s), $((t - n)) waived ($((t - i)) inline)"
}

# A tool that fails, or stops before the step its findings come from, fails its
# phase: "0 finding(s)" must not stand for "not checked"
failed() {  # $1 phase, $2 reason, $3 its log (default $W/<phase>.log)
  RESULT[$1]="FAILED ($2), see ${3:-$W/$1.log}"
  echo "-- $1: FAILED, $2"
}

# after found: a tool that exits non-zero must have reported something (waived
# or not); a non-zero exit with nothing to show is the tool failing
exit_check() {  # $1 phase, $2 exit status
  if [ "$2" -ne 0 ] && [ ! -s "$1.all" ]; then failed "$1" "exit $2, nothing reported"; fi
}

# Xilinx primitive stubs, for the tags, Verilator, Yosys and slang phases
bb() {
  [ -s bb.sv ] && return 0
  if [ -n "${XILINX_BB:-}" ]; then cp "$XILINX_BB" bb.sv
  elif have "$YOSYS_CONFIG"; then
    "$PYTHON" "$HERE/gen_xilinx_bb.py" "$("$YOSYS_CONFIG" --datdir)/xilinx" bb.sv $PRIMS >/dev/null
  else return 1; fi
}

# /*LINT:OK*/ on anything but an output port; the port directions come from the
# module headers of the design, the shims and the primitive stubs, plus the
# other CPU core: soc_top.sv instantiates both, behind `ifdef
run_tags() {
  bb || { skip tags "no Xilinx stubs (yosys-config or XILINX_BB)"; return; }
  "$PYTHON" - $OTHER_CPU_SV -- bb.sv $SHIMS $SV > tags.log <<'EOF'
import re, sys
ports_only = sys.argv[1:sys.argv.index('--')]
files = sys.argv[sys.argv.index('--') + 1:]
text = {f: open(f).read() for f in ports_only + files}
types = {'logic', 'wire', 'reg', 'signed', 'unsigned', 'var', 'bit', 'integer', 'tri'}
dirs = {}
for t in text.values():
    t = re.sub(r'//[^\n]*|/\*.*?\*/', '', t, flags=re.S)
    for m in re.finditer(r'\bmodule\s+(\w+)(.*?)\bendmodule', t, flags=re.S):
        d = dirs.setdefault(m.group(1), {})
        for p in re.finditer(r'\b(input|output|inout)\b(.*?)(?=\b(?:input|output|inout)\b|[;()]|$)',
                             re.sub(r'\[[^\]]*\]', ' ', m.group(2)), flags=re.S):
            for part in re.sub(r'\[[^\]]*\]', ' ', p.group(2)).split(','):
                w = [x for x in re.findall(r'[A-Za-z_]\w*', part) if x not in types]
                if w:
                    d.setdefault(w[-1], p.group(1))
tag  = re.compile(r'\.(\w+)\s*\(\s*/\*LINT:OK\*/\s*\)')
inst = re.compile(r'\s*([A-Za-z_]\w*)\s*(#|\s\w+\s*\()')
kw   = {'if', 'else', 'for', 'begin', 'end', 'case', 'assign', 'module', 'always', 'always_ff',
        'always_comb', 'initial', 'function', 'task', 'return', 'generate'}
for f in files:
    lines = text[f].split('\n')
    for n, line in enumerate(lines, 1):
        for m in tag.finditer(line):
            typ = next((t.group(1) for i in range(n - 1, -1, -1)
                        for t in [inst.match(lines[i])] if t and t.group(1) not in kw), None)
            d = dirs.get(typ, {}).get(m.group(1))
            if d != 'output':
                print(f'{f}:{n}: LINT:OK on {d or "unknown"} port {typ}.{m.group(1)}')
# our RTL rule: every unique and priority case has a default, also a full one
# (no tool checks it: Verible's case-missing-default skips unique, Verilator
# and slang only flag values no item covers); unique0 has none
tok = re.compile(r'\b(?:(unique0|unique|priority)\s+)?(case[zx]?|endcase|default)\b|([{}])')
blank = re.compile(r'"(?:\\.|[^"\\\n])*"|//[^\n]*|/\*.*?\*/', re.S)
for f in files:
    if re.search(r'picorv32|^csr(_pkg)?\.sv$|\.v$', f):  # PicoRV32, the PeakRDL CSR, stubs
        continue
    # strings and comments blanked, newlines kept, so line numbers stay
    t = blank.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text[f])
    stack, braces = [], 0  # one [qualifier, line, brace depth, has default] per open case
    for m in tok.finditer(t):
        if m.group(3):
            braces += 1 if m.group(3) == '{' else -1
        elif m.group(2).startswith('case'):
            stack.append([m.group(1), t.count('\n', 0, m.start()) + 1, braces, False])
        elif m.group(2) == 'default':
            # a case label, not '{default: ...} in an assignment pattern
            if stack and stack[-1][2] == braces:
                stack[-1][3] = True
        elif stack:
            q, n, _, has = stack.pop()
            if q in ('unique', 'priority') and not has:
                print(f'{f}:{n}: {q} case without default')
    for q, n, _, _ in stack:
        print(f'{f}:{n}: case without endcase (an `ifdef around case or endcase?)')
EOF
  local rc=$?
  [ $rc -eq 0 ] || { failed tags "exit $rc"; return; }
  found tags < tags.log
}

run_verilator() {
  have "$VERILATOR" || { skip verilator "$VERILATOR not found"; return; }
  bb || { skip verilator "no Xilinx stubs (yosys-config or XILINX_BB)"; return; }
  cat > waive.vlt <<'EOF'
`verilator_config
lint_off -file "*bb.sv"
lint_off -file "*_shim.v"
EOF
  "$VERILATOR" --lint-only -Wall -Wno-fatal --top-module $TOP -DSYN_YOSYS_BUG -DPCIE_LANES=$LANES ${GEN:+-DPCIE_GEN=$GEN} $VL_DEFS \
    -Isrc -I$P waive.vlt bb.sv $SV $SHIMS > verilator.log 2>&1
  local rc=$?
  found verilator < <(grep -E '^%(Warning|Error)' verilator.log)
  exit_check verilator $rc
}

run_yosys() {
  have "$YOSYS" || { skip yosys "$YOSYS not found"; return; }
  bb || { skip yosys "no Xilinx stubs"; return; }
  "$YOSYS" $YOSYS_ARGS -q -l yosys.log -p "read_slang -D SYN_YOSYS_BUG -D PCIE_LANES=$LANES ${GEN:+-D PCIE_GEN=$GEN} $SL_DEFS --top $TOP -I $P -I src \
      bb.sv $SV $SHIMS; hierarchy -check -top $TOP; proc; check -nolatches -initdrv" \
      >/dev/null 2>&1
  local rc=$?
  if [ $rc -ne 0 ] || ! grep -q 'Executing CHECK pass' yosys.log; then
    failed yosys "exit $rc, CHECK pass not reached"; return
  fi
  # read_slang's own warnings are the slang phase's business; take check's
  found yosys < <(awk '/Executing CHECK pass/ { c = 1 } c && /^Warning:/' yosys.log)
}

# Our rule: every control flop and flag has a reset; data and address flops
# need none. A flop with no reset that reaches an enable, a select or a reset
# starts at X in a 4-state simulator and can lock the design up (eduBOS5
# dmem_vld). One finding per flop, at its source line; the rules are in
# run-lint-reset.py.
run_reset() {
  have "$YOSYS" || { skip reset "$YOSYS not found"; return; }
  bb || { skip reset "no Xilinx stubs"; return; }
  "$YOSYS" $YOSYS_ARGS -q -l reset.log -p "read_slang -D SYN_YOSYS_BUG -D PCIE_LANES=$LANES ${GEN:+-D PCIE_GEN=$GEN} $SL_DEFS --top $TOP -I $P -I src \
      bb.sv $SV $SHIMS; hierarchy -check -top $TOP; proc; flatten; opt_expr; opt_clean; opt_dff; opt_clean; write_json reset.json" \
      >/dev/null 2>&1
  local rc=$?
  [ $rc -eq 0 ] && [ -s reset.json ] || { failed reset "exit $rc, no netlist"; return; }
  "$PYTHON" "$HERE/run-lint-reset.py" reset.json > reset.out 2>> reset.log
  local rc2=$?
  [ $rc2 -eq 0 ] || { failed reset "netlist check exit $rc2"; return; }
  # eduBOS5 alone with the options no SoC build sets: RV32M (in RTL, the
  # DSP48E1 is a primitive) and the counters
  "$YOSYS" $YOSYS_ARGS -q -l reset.cpu.log -p "read_slang --top edubos5 -G USE_HW_MUL=1 -G EN_COUNTERS=1 \
      -I src $(echo $EDUBOS5_SV); hierarchy -check -top edubos5; proc; flatten; opt_expr; opt_clean; opt_dff; opt_clean; \
      write_json reset.cpu.json" >/dev/null 2>&1
  rc=$?
  [ $rc -eq 0 ] && [ -s reset.cpu.json ] || { failed reset "eduBOS5 RV32M: exit $rc, no netlist"; return; }
  "$PYTHON" "$HERE/run-lint-reset.py" reset.cpu.json >> reset.out 2>> reset.log ||
    { failed reset "eduBOS5 RV32M: netlist check failed"; return; }
  found reset < <(sort -u reset.out)
}

# A memory mapped to block or LUT RAM must need no read-first or transparency
# logic in fabric: Yosys builds it there, saying so only in its debug log.
# synth_xilinx up to its memory mapping, then that step under debug, so the
# libraries and options are synth_xilinx's own. The memories' source lines
# come from a netlist written before it (after it, the simulation models it
# reads stop write_json). One finding per memory, at its source line; the
# rules are in run-lint-ram.py.
run_ram() {
  have "$YOSYS" || { skip ram "$YOSYS not found"; return; }
  bb || { skip ram "no Xilinx stubs"; return; }
  local syn="synth_xilinx -family xc7 -top $TOP"
  "$YOSYS" $YOSYS_ARGS -q -l ram.log -p "read_slang -D SYN_YOSYS_BUG -D PCIE_LANES=$LANES ${GEN:+-D PCIE_GEN=$GEN} $SL_DEFS --top $TOP -I $P -I src \
      bb.sv $SV $SHIMS; hierarchy -check -top $TOP; proc; write_json ram.json; \
      $syn -run begin:map_memory; debug $syn -run map_memory:map_ffram" \
      >/dev/null 2>&1
  local rc=$?
  [ $rc -eq 0 ] && [ -s ram.json ] && grep -q 'Executing MEMORY_LIBMAP' ram.log ||
    { failed ram "exit $rc, memory_libmap not reached"; return; }
  "$PYTHON" "$HERE/run-lint-ram.py" ram.log ram.json > ram.out 2>> ram.log ||
    { failed ram "log check failed"; return; }
  found ram < <(sort -u ram.out)
}

# IOB flops: every data pin of the top reaches its flop directly, so Vivado
# and nextpnr can put the flop in the I/O tile (run-lint-iob.py). Uses the
# reset phase's netlist when this run made one
run_iob() {
  have "$YOSYS" || { skip iob "$YOSYS not found"; return; }
  bb || { skip iob "no Xilinx stubs"; return; }
  local rc
  if [[ " $PHASES " != *" reset "* ]] || [ ! -s reset.json ]; then
    "$YOSYS" $YOSYS_ARGS -q -l iob.log -p "read_slang -D SYN_YOSYS_BUG -D PCIE_LANES=$LANES ${GEN:+-D PCIE_GEN=$GEN} $SL_DEFS --top $TOP -I $P -I src \
        bb.sv $SV $SHIMS; hierarchy -check -top $TOP; proc; flatten; opt_expr; opt_clean; opt_dff; opt_clean; write_json reset.json" \
        >/dev/null 2>&1
    rc=$?
    [ $rc -eq 0 ] && [ -s reset.json ] || { failed iob "exit $rc, no netlist"; return; }
  fi
  "$PYTHON" "$HERE/run-lint-iob.py" reset.json > iob.out 2>> iob.log ||
    { failed iob "netlist check failed"; return; }
  found iob < <(sort -u iob.out)
}

run_slang() {
  have "$YOSYS" || { skip slang "$YOSYS not found"; return; }
  bb || { skip slang "no Xilinx stubs"; return; }
  "$YOSYS" $YOSYS_ARGS -q -l slang.log -p "read_slang -D SYN_YOSYS_BUG -D PCIE_LANES=$LANES ${GEN:+-D PCIE_GEN=$GEN} $SL_DEFS --top $TOP -I $P -I src \
      -Wextra bb.sv $SV $SHIMS" >/dev/null 2>&1
  local rc=$?
  # Yosys' built-in read_slang prints "file:line:col: warning:", the yosys-slang
  # plugin (OSS CAD Suite, CI) prints "file:line: Warning:"; bring both to the
  # first form so the waivers match either
  found slang < <(sed -E 's/^([^ :]+:[0-9]+): Warning:/\1:0: warning:/; s/^([^ :]+:[0-9]+): Error:/\1:0: error:/' slang.log |
                  grep -E 'warning:|error:')
  exit_check slang $rc
}

run_verible() {
  have "$VERIBLE" || { skip verible "$VERIBLE not found"; return; }
  cp "$HERE/run-lint.verible" .
  "$VERIBLE" --rules_config=run-lint.verible $SV > verible.log 2>&1
  local rc=$?
  [ $rc -le 1 ] || { failed verible "exit $rc"; return; }
  found verible < <(grep -E ':[0-9]+:[0-9]+' verible.log)
  exit_check verible $rc
}

# Vivado has its own XPM and UNIMACRO libraries, so no shims here. One
# synthesis serves the cdc, check-timing and methodology phases (about 3 min);
# VIVADO_RC keeps its outcome
VIVADO_RC=
vivado_run() {
  [ -n "$VIVADO_RC" ] && return $VIVADO_RC
  cat > vivado.tcl <<EOF
read_verilog -sv {$SV}
read_xdc [glob xdc/*.xdc]
synth_design -top $TOP -part $PART -flatten_hierarchy rebuilt -verilog_define PCIE_LANES=$LANES${GEN:+ -verilog_define PCIE_GEN=$GEN}$VIV_DEFS
report_cdc -details -file cdc.rpt
report_clock_interaction -file clock_interaction.rpt
check_timing -verbose -file check_timing.rpt
report_methodology -file methodology.rpt
EOF
  rm -f cdc.rpt check_timing.rpt methodology.rpt
  "$VIVADO" -mode batch -nojournal -log vivado.log -source vivado.tcl >/dev/null 2>&1
  VIVADO_RC=$?
  echo "exit $VIVADO_RC" > vivado.err
  [ $VIVADO_RC -eq 0 ] && [ -s cdc.rpt ] && [ -s check_timing.rpt ] && [ -s methodology.rpt ] ||
    { [ $VIVADO_RC -eq 0 ] && VIVADO_RC=3; echo "exit $VIVADO_RC, a report is missing" > vivado.err; }
  return $VIVADO_RC
}

vivado_phase() {  # $1 phase: skip without Vivado, else run it once
  have "$VIVADO" || { skip $1 "$VIVADO not found"; return 1; }
  vivado_run || { failed $1 "Vivado: $(cat vivado.err)" "$W/vivado.log"; return 1; }
}

run_cdc() {
  vivado_phase cdc || return
  # detail rows: <row> CDC-<n> <severity> <description> ... <from> <to>
  found cdc < <(grep -E '^ +[0-9]+ +CDC-[0-9]+ +(Critical|Warning) ' cdc.rpt)
}

# check_timing: every HIGH or MEDIUM line, as "<check>: <line>" (no_clock,
# unconstrained_internal_endpoints, no_input_delay, multiple_clock, loops, ...)
run_check_timing() {
  vivado_phase check-timing || return
  found check-timing < <(awk '/^[0-9]+\. checking / { chk = $3 }
      /\((HIGH|MEDIUM)\) *$/ { l = $0; sub(/^ +/, "", l); print chk ": " l }' check_timing.rpt)
}

# report_methodology: every Critical Warning and Warning, as
# "<rule> <severity>: <message>" (TIMING-17 Critical Warning: The clock pin ...);
# and every CRITICAL WARNING of the Vivado run that points at an XDC line
# (a constraint on nothing: "'set_property' expects at least one object"), as
# "XDC <file>:<line>: <message>"
run_methodology() {
  vivado_phase methodology || return
  found methodology < <(awk '/^[A-Z]+-[0-9]+#[0-9]+ (Critical Warning|Warning) *$/ {
      r = $1; sub(/#.*/, "", r); s = $0; sub(/^[^ ]+ /, "", s); sub(/ +$/, "", s)
      getline; getline m; sub(/ +$/, "", m); print r " " s ": " m }' methodology.rpt
    sed -n -E 's#^CRITICAL WARNING: (.*) \[([^]]*/)?([^]/]+\.xdc):([0-9]+)\]$#XDC \3:\4: \1#p' vivado.log)
}

for ph in $PHASES; do
  echo "== $ph"; run_${ph//-/_}  # names checked at start-up
done

echo; echo "== summary  (x$LANES${GEN:+ Gen$GEN}, $W)"
FAIL=0
for ph in $PHASES; do
  printf '  %-14s %s\n' "$ph" "${RESULT[$ph]}"
  case ${RESULT[$ph]} in skipped*|"0 finding"*) ;; *) FAIL=1 ;; esac
done
exit $FAIL

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------
