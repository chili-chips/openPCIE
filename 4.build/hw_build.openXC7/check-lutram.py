# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# After synthesis: no large memory in distributed RAM (LUTs). A memory that
# takes more than LUTRAM_MAX_LUTS LUTs belongs in block RAM; when Yosys cannot
# put it there (its read is not registered, e.g. no output flop it can merge),
# the build fails here instead of quietly spending thousands of LUTs.
#
# usage:  python3 check-lutram.py <top.json> [max LUTs per memory, default 64]
#   or LUTRAM_MAX_LUTS=<n> in the environment
#
# The cells of one memory are named after it: <memory>.<i>.<j>

import json, os, re, sys
from collections import Counter

LUTS = {'RAM16X1S': 1, 'RAM32X1S': 1, 'RAM64X1S': 1, 'RAM128X1S': 2, 'RAM256X1S': 4,
        'RAM16X1D': 2, 'RAM32X1D': 2, 'RAM64X1D': 2, 'RAM128X1D': 4,
        'RAM32M': 4, 'RAM64M': 4, 'RAM32M16': 8, 'RAM64M8': 8}
limit = int(sys.argv[2] if len(sys.argv) > 2 else os.environ.get('LUTRAM_MAX_LUTS', 64))

mods = json.load(open(sys.argv[1]))['modules']
top = next(m for m in mods.values() if m.get('attributes', {}).get('top'))
use = Counter()
for name, c in top['cells'].items():
    if c['type'] in LUTS:
        use[re.sub(r'(\.\d+)+$', '', name)] += LUTS[c['type']]

bad = {m: n for m, n in use.items() if n > limit}
for m, n in sorted(use.items()):
    print(f"lutram: {m}: {n} LUTs{'  <-- over ' + str(limit) + ': belongs in block RAM' if n > limit else ''}")
if bad:
    print(f'lutram: FAIL, {len(bad)} memories over {limit} LUTs in distributed RAM', file=sys.stderr)
    sys.exit(1)
print(f'lutram: OK, {sum(use.values())} LUTs as distributed RAM, none over {limit} per memory')

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# -----------------------------------------------------------------------------
