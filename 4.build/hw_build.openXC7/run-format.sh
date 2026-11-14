#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Format the opensource RTL (2.rtl/0.common.opensource/src) in the project style:
#   1. every if, else, always, for, foreach, while, forever and initial body is
#      a begin ... end block, also when it is a single statement (the statement
#      positions come from the verible-verilog-syntax tree);
#   2. verible-verilog-format: Verible defaults (100-column limit) plus every
#      alignment option set to align, end and else on separate lines, LF line
#      endings. It does not wrap lines over the limit: wrap those by hand;
#   2c. at least one space before the '(' of a named parameter or port
#      connection: .NAME (signal) (run-format-conn.py; Verible puts the '('
#      against the longest name of a block);
#   2d. a logic declaration next to the assign that drives it: in a block of
#      adjacent logic and assign lines the names share a column, and so do
#      the '=' (run-format-align.py);
#   2e. a module's port list is one alignment: the direction, type,
#      dimension, name and comment columns are the same in every group of
#      ports (run-format-align.py; Verible aligns each group on its own);
#   0. no CR (^M) in these files (removed unless --check);
#   3. no ?: in a port connection (reported, not fixed).
#
# usage:  bash run-format.sh            format every RTL file in place
#         bash run-format.sh --check    only report the files that are not
#                                       formatted; exit 1 if there are any
#
# Needs python3 and Verible (VERIBLE_FORMAT, VERIBLE_SYNTAX, VERIBLE_LINT can
# override the commands). Generated code (the PeakRDL csr.sv / csr_pkg.sv in
# 4.build/csr_build) is left as the generator writes it.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../2.rtl/0.common.opensource/src
VERIBLE_FORMAT=${VERIBLE_FORMAT:-verible-verilog-format}
VERIBLE_SYNTAX=${VERIBLE_SYNTAX:-verible-verilog-syntax}
VERIBLE_LINT=${VERIBLE_LINT:-verible-verilog-lint}

STYLE=(
  --line_terminator=LF
  --column_limit=100
  --assignment_statement_alignment=align
  --case_items_alignment=align
  --distribution_items_alignment=align
  --enum_assignment_statement_alignment=align
  --formal_parameters_alignment=align
  --module_net_variable_alignment=align
  --named_parameter_alignment=align
  --named_port_alignment=align
  --parameter_declaration_alignment=align
  --port_declarations_alignment=align
  --struct_union_members_alignment=align
  --wrap_end_else_clauses=true
)

for t in python3 "$VERIBLE_FORMAT" "$VERIBLE_SYNTAX" "$VERIBLE_LINT"; do
  command -v "$t" >/dev/null || { echo "run-format: $t not found" >&2; exit 2; }
done

MODE=format
[ "${1:-}" = "--check" ] && MODE=check

# Step 1: begin ... end around single-statement bodies. The syntax tree leaves
# out code under an undefined `ifdef, so a file is parsed as is, with all its
# `ifdef/`ifndef/`elsif macros defined, and once per macro with only that one
# defined (a `define put in front). --count only reports the number of bodies.
explicit_begin() {
  VERIBLE_SYNTAX="$VERIBLE_SYNTAX" python3 - "$@" <<'PY'
import json, os, re, subprocess, sys, tempfile

LOOPS = ('kForLoopStatement', 'kWhileLoopStatement', 'kForeachLoopStatement',
         'kForeverLoopStatement', 'kInitialStatement')

def leaves(n):
    if 'children' in n:
        for c in n['children']:
            if c is not None: yield from leaves(c)
    elif 'start' in n:
        yield n

def span(n):
    ls = list(leaves(n)); return ls[0]['start'], ls[-1]['end']

def bodies(n, parent, out):
    t, ks = n.get('tag'), [c for c in n.get('children', []) if c is not None]
    b = ks[-1] if ks else None
    if b is not None and b.get('tag') != 'kSeqBlock':
        if t == 'kIfBody' or (t == 'kElseBody' and b.get('tag') != 'kConditionalStatement') \
           or (t == 'kProceduralTimingControlStatement' and parent == 'kAlwaysStatement') \
           or (t in LOOPS and b.get('tag') != 'kGenerateBlock'):
            out.add(span(b))
    for c in ks:
        bodies(c, t, out)

def parse(data):
    with tempfile.NamedTemporaryFile(suffix='.sv', delete=False) as f:
        f.write(data)
    try:
        r = subprocess.run([os.environ['VERIBLE_SYNTAX'], '--export_json', '--printtree', f.name],
                           capture_output=True, text=True)
        tree = list(json.loads(r.stdout).values())[0].get('tree')
    finally:
        os.unlink(f.name)
    out = set()
    if tree: bodies(tree, None, out)
    return out

def views(data):
    """the file as is, all macros defined, each macro defined alone: (text, prefix length)"""
    names = sorted(set(re.findall(rb'`(?:ifn?def|elsif)\s+(\w+)', data)))
    sets = [[]] + ([names] if len(names) > 1 else []) + [[n] for n in names]
    for ns in sets:
        pre = b''.join(b'`define ' + n + b'\n' for n in ns)
        yield pre + data, len(pre)

count_only = sys.argv[1] == '--count'
total = 0
for path in sys.argv[2:]:
    data = open(path, 'rb').read()
    spans = set()
    for text, n in views(data):
        spans |= {(a - n, e - n) for a, e in parse(text) if a >= n}
    total += len(spans)
    if count_only or not spans:
        if spans: print('needs begin/end: %d body(ies) in %s' % (len(spans), os.path.basename(path)))
        continue
    ins = []
    for a, e in spans:
        ins.append((a, b'begin '))
        m = re.match(rb'[ \t]*//[^\n]*', data[e:])      # keep a trailing comment on its statement
        ins.append((e + m.end(), b'\nend') if m else (e, b' end'))
    for p, txt in sorted(ins, key=lambda x: x[0], reverse=True):
        data = data[:p] + txt + data[p:]
    open(path, 'wb').write(data)
print('-- explicit-begin: %d body(ies) %s' % (total, 'without begin/end' if count_only else 'wrapped'))
sys.exit(1 if count_only and total else 0)
PY
}

FILES=("$SRC"/*.sv "$SRC"/pcie/*.sv "$SRC"/CPU.eduBOS5/*.sv)
bad=0

# Step 0: CR (^M) in these files: reported with --check, removed otherwise
CR_FILES=($(grep -l $'\r' "${FILES[@]}"))
for f in "${CR_FILES[@]}"; do
  if [ $MODE = check ]; then echo "CR line endings: ${f##*/}"; else sed -i 's/\r//g' "$f"; fi
done
ncr=${#CR_FILES[@]}
if [ $MODE = check ]; then
  echo "-- CR: $ncr file(s) with CR"
else
  echo "-- CR: removed from $ncr file(s)"; ncr=0
fi
if [ $MODE = check ]; then
  explicit_begin --count "${FILES[@]}" || bad=$((bad + 1))
else
  explicit_begin --apply "${FILES[@]}" || bad=$((bad + 1))
fi

# Step 2: verible-verilog-format. It can exit 0 on a file it cannot parse, so a
# message on stderr also counts as a failure.
nf=0
for f in "${FILES[@]}"; do
  if [ $MODE = check ]; then
    # what steps 2 to 2e would make of it, compared with the file
    tmp=$(mktemp --suffix=.sv)
    err=$("$VERIBLE_FORMAT" "${STYLE[@]}" "$f" 2>&1 >"$tmp"); rc=$?
    [ $rc -eq 0 ] && [ -z "$err" ] && python3 "$HERE/run-format-conn.py" "$tmp" &&
      python3 "$HERE/run-format-align.py" "$tmp" && cmp -s "$f" "$tmp"
    if [ $? -ne 0 ]; then echo "not formatted: ${f#$SRC/}"; nf=$((nf + 1)); fi
    rm -f "$tmp"
  else
    err=$("$VERIBLE_FORMAT" "${STYLE[@]}" --inplace "$f" 2>&1); rc=$?
    if [ $rc -ne 0 ] || [ -n "$err" ]; then echo "failed: ${f#$SRC/}: $(echo "$err" | head -1)" >&2; nf=$((nf + 1)); fi
    python3 "$HERE/run-format-conn.py" "$f"
    python3 "$HERE/run-format-align.py" "$f"
  fi
done

# Cross-check with the Verible lint rule for the same thing
lint_out=$("$VERIBLE_LINT" --ruleset=none --rules=explicit-begin "${FILES[@]}" 2>&1); lrc=$?
nl=$(printf '%s\n' "$lint_out" | grep -c 'explicit-begin')
# exit 0: clean, 1: findings; anything else, or 1 with nothing found, is the
# tool failing (a syntax error, a bad option), never "0 findings"
lint_failed=0
if [ $lrc -gt 1 ] || { [ $lrc -eq 1 ] && [ $nl -eq 0 ]; }; then lint_failed=1; bad=$((bad + 1)); fi

if [ $MODE = check ]; then
  echo "-- verible-format: $nf file(s) not formatted"
else
  echo "-- verible-format: done, $nf failure(s)"
fi
if [ $lint_failed -eq 1 ]; then
  echo "-- verible-lint explicit-begin: FAILED (exit $lrc): $(printf '%s\n' "$lint_out" | head -1)"
else
  echo "-- verible-lint explicit-begin: $nl finding(s)"
fi

# Lines over the 100-column limit (the formatter leaves them as they are)
awk 'length > 100 { sub(".*/src/", "", FILENAME); print "over 100 columns: " FILENAME ":" FNR }' "${FILES[@]}"
nc=$(awk 'length > 100' "${FILES[@]}" | wc -l)
echo "-- line length: $nc line(s) over 100 columns"

# A ?: in a port connection: give it a signal and a continuous assign instead
# (as VHDL port maps require). Reported in both modes; the fix needs a name,
# so it is not automatic.
port_out=$(perl -e '
  for my $f (@ARGV) {
    open my $h, "<", $f or next; my $s = do { local $/; <$h> };
    $s =~ s{//[^\n]*}{}g; $s =~ s{/\*.*?\*/}{ my $c = $&; $c =~ s/[^\n]//g; $c }gse;
    while ($s =~ /\.\w+\s*\(/g) {
      my ($start, $d, $i) = ($-[0], 1, pos $s);
      $d += (substr($s, $i, 1) eq "(") - (substr($s, $i, 1) eq ")"), $i++ while $d && $i < length $s;
      next unless substr($s, pos($s), $i - pos($s)) =~ /\?/;
      my $line = 1 + (substr($s, 0, $start) =~ tr/\n//);
      (my $r = $f) =~ s{.*/}{};
      print "?: in a port connection: $r:$line\n";
    }
  }' "${FILES[@]}")
[ -n "$port_out" ] && printf '%s\n' "$port_out"
np=$(printf '%s' "$port_out" | grep -c '?:')
echo "-- port connections: $np with ?:"
[ $bad -eq 0 ] && [ $ncr -eq 0 ] && [ $nf -eq 0 ] && [ $nl -eq 0 ] && [ $nc -eq 0 ] && [ $np -eq 0 ]

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------
