# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# After synthesis: every memory in distributed RAM (LUTRAM) has the shape
# LUTRAM is for. The form factor decides, not the LUT count alone:
#   - shallow (depth <= LUTRAM_MAX_DEPTH, default 64): LUTRAM, at any width.
#     A block RAM is at most 72 bits wide and 512 deep (simple dual port), so a
#     shallow, wide memory would take one RAMB18 per 36 bits and fill <= 1/8 of
#     its rows; LUTRAM also gives the combinational read FIFOs and tables need
#   - deep (depth > LUTRAM_MAX_DEPTH) and big (> LUTRAM_MAX_LUTS LUTs, default
#     64): belongs in block RAM. When Yosys cannot put it there (its read is not
#     registered), the build fails here instead of quietly spending LUTs
#
# usage:  python3 check-lutram.py <top.json> [max LUTs] [max depth]
#   or LUTRAM_MAX_LUTS=<n> LUTRAM_MAX_DEPTH=<n> in the environment
#
# Read from the mapped cells. The cells of one memory are named after it,
# <memory>.<i>.<j>. Width = the distinct data-in nets (read-port copies share
# them). Depth = the rows a cell addresses (2^address bits that are not
# constant) times the banks (cells holding the same bit under another write
# enable or write address). A cell's rows are a power of two, so 3 rows count as 4.

import json, os, re, sys
from collections import defaultdict

# type: (LUTs, rows, data-in pins, write-address pins)
PRIM = {
    'RAM16X1S': (1, 16, ['D'], ['A0', 'A1', 'A2', 'A3']),
    'RAM32X1S': (1, 32, ['D'], ['A0', 'A1', 'A2', 'A3', 'A4']),
    'RAM64X1S': (1, 64, ['D'], ['A0', 'A1', 'A2', 'A3', 'A4', 'A5']),
    'RAM128X1S': (2, 128, ['D'], ['A0', 'A1', 'A2', 'A3', 'A4', 'A5', 'A6']),
    'RAM256X1S': (4, 256, ['D'], ['A']),
    'RAM16X1D': (2, 16, ['D'], ['A0', 'A1', 'A2', 'A3']),
    'RAM32X1D': (2, 32, ['D'], ['A0', 'A1', 'A2', 'A3', 'A4']),
    'RAM64X1D': (2, 64, ['D'], ['A0', 'A1', 'A2', 'A3', 'A4', 'A5']),
    'RAM128X1D': (4, 128, ['D'], ['A']),
    'RAM32M': (4, 32, ['DIA', 'DIB', 'DIC', 'DID'], ['ADDRD']),
    'RAM64M': (4, 64, ['DIA', 'DIB', 'DIC', 'DID'], ['ADDRD']),
    'RAM32M16': (8, 32, ['DI' + p for p in 'ABCDEFGH'], ['ADDRH']),
    'RAM64M8': (8, 64, ['DI' + p for p in 'ABCDEFGH'], ['ADDRH']),
}
arg = lambda i, env, d: int(sys.argv[i] if len(sys.argv) > i else os.environ.get(env, d))
max_luts = arg(2, 'LUTRAM_MAX_LUTS', 64)
max_depth = arg(3, 'LUTRAM_MAX_DEPTH', 64)

mods = json.load(open(sys.argv[1]))['modules']
top = next(m for m in mods.values() if m.get('attributes', {}).get('top'))
net = lambda b: b if isinstance(b, int) else None  # '0'/'1'/'x' are constants

luts = defaultdict(int)
rows = defaultdict(int)
holders = defaultdict(lambda: defaultdict(set))  # memory -> data-in net -> {(we, waddr)}
for name, c in top['cells'].items():
    if c['type'] not in PRIM:
        continue
    n_luts, n_rows, din, wa = PRIM[c['type']]
    mem = re.sub(r'(\.\d+)+$', '', name)
    cc = c['connections']
    addr = [b for p in wa for b in cc.get(p, [])]
    live = sum(1 for b in addr if net(b) is not None)
    luts[mem] += n_luts
    rows[mem] = max(rows[mem], min(n_rows, 1 << live))
    port = (tuple(cc.get('WE', [])), tuple(addr))
    for p in din:
        for b in cc.get(p, []):
            if net(b) is not None:
                holders[mem][b].add(port)

bad = []
for mem in sorted(luts):
    width = len(holders[mem])
    banks = max((len(s) for s in holders[mem].values()), default=1)
    depth = rows[mem] * banks
    fail = luts[mem] > max_luts and depth > max_depth
    bad += [mem] if fail else []
    note = '  <-- deep and big: belongs in block RAM' if fail else ''
    print(f'lutram: {mem}: {depth} x {width}, {luts[mem]} LUTs{note}')
if bad:
    print(f'lutram: FAIL, {len(bad)} memories deeper than {max_depth} and over {max_luts} LUTs'
          ' in distributed RAM', file=sys.stderr)
    sys.exit(1)
print(f'lutram: OK, {sum(luts.values())} LUTs as distributed RAM; none deeper than {max_depth}'
      f' and over {max_luts} LUTs')

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# -----------------------------------------------------------------------------
