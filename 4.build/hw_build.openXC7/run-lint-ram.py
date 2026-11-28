#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The ram phase of run-lint.sh: a memory that Yosys maps to block or LUT RAM
# but must wrap in fabric logic to emulate read-first or transparent
# (write-first) behaviour the RAM primitive lacks. Such a memory, one that may
# read and write the same address in one clock, costs flip-flops, address
# compares and a data mux (72 DFF, 76 LUT for eduBOS5's 32x32 register file on
# GateMate, whose block RAM has neither mode). Fix: "write, else read"
# (openCologne-PCIE 4.build/README.md, "Block RAM without read-first").
# What each RAM has natively, so what can be flagged:
#   GateMate block RAM   neither: read-first and transparency both flagged
#   Xilinx block RAM     both (READ_FIRST, WRITE_FIRST): nothing flagged
#   Xilinx LUT RAM       read-first only (asynchronous read, then a
#                        register): transparency flagged. Fix: accept the
#                        old data, or let the memory go to block RAM
# Only the mapping Yosys chose counts, per read port. On LUT RAM, a port whose
# address register Yosys took back out ("Extracted addr FF") costs nothing:
# that is the RTL's asynchronous read, so its emulation is not a finding. One
# whose data register it took out ("Extracted data FF") needs the bypass.
# The same file in openPCIE, openCologne-PCIE and wireguard-fpga.
#
# usage: run-lint-ram.py <memory_libmap debug log> <netlist.json>
#   the log of "debug memory_libmap" (or of a synth script under debug), the
#   JSON written just before it (for each memory's source line). One finding
#   per memory on stdout:
#     <file>:<line>: memory <name> mapped to <cell>: emulates <what>

import json
import re
import sys

log_path, json_path = sys.argv[1], sys.argv[2]

# source line of each memory, by its name in the log (<module>.<memid>)
src = {}
with open(json_path) as f:
    netlist = json.load(f)
for mod_name, mod in netlist.get("modules", {}).items():
    for cell in mod.get("cells", {}).values():
        if not cell["type"].startswith("$mem"):
            continue
        memid = cell["parameters"].get("MEMID", "").lstrip("\\")
        s = cell.get("attributes", {}).get("src", "")
        # "file:l1.c1-l2.c2|..." -> file:l1
        m = re.match(r"([^:|]+):(\d+)", s)
        src[f"{mod_name}.{memid}"] = f"{m.group(1)}:{m.group(2)}" if m else "?:0"

# candidates per memory: cell type -> the (read port, emulation) it needs
cands = {}  # memory -> {cell: set()}
chosen = {}  # memory -> cell
addr_ff = {}  # memory -> read ports whose address FF was extracted
seen = 0  # "mapping candidates" blocks parsed
mem = cell = port = None
with open(log_path) as f:
    for line in f:
        line = line.rstrip("\n")
        m = re.match(r"Memory (\S+) mapping candidates", line)
        if m:
            mem, cell, port = m.group(1), None, None
            cands[mem] = {}
            seen += 1
            continue
        m = re.match(r"- (\$\S+):$", line)
        if m and mem:
            cell, port = m.group(1), None
            cands[mem].setdefault(cell, set())
            continue
        if re.match(r"- logic fallback", line):
            cell = None
            continue
        m = re.match(r"\s+- read port (\d+):", line)
        if m:
            port = int(m.group(1))
            continue
        m = re.match(r"\s+- emulate (read-first behavior|transparency)", line)
        if m and mem and cell:
            what = "read-first" if "read-first" in m.group(1) else "transparency"
            cands[mem][cell].add((port, what))
            continue
        m = re.match(r"mapping memory (\S+) via (\S+)", line)
        if m:
            chosen[m.group(1)] = m.group(2)
            continue
        m = re.match(r"Extracted addr FF from read port (\d+) of (\S+):", line)
        if m:
            addr_ff.setdefault(m.group(2), set()).add(int(m.group(1)))

# memories in the netlist but no candidates parsed: the log wording changed,
# or debug did not reach the log. Fail rather than report 0 findings
if src and not seen:
    sys.exit("run-lint-ram.py: no memory_libmap candidates in the log")

for name, c in sorted(chosen.items()):
    need = cands.get(name, {}).get(c, set())
    if "LUTRAM" in c:
        need = {(p, w) for (p, w) in need if p not in addr_ff.get(name, set())}
    need = {w for (p, w) in need}
    if need:
        print(f"{src.get(name, '?:0')}: memory {name} mapped to {c}: emulates "
              f"{' and '.join(sorted(need))} in fabric (write, else read)")
