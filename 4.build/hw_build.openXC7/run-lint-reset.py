# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The reset phase of run-lint.sh. Our rule: every control flop and flag has a
# reset; data and address flops need none.
#
# usage:  python3 run-lint-reset.py <netlist.json>
#   the netlist: Yosys write_json after proc; flatten; opt_expr; opt_clean;
#   opt_dff (a hold is an enable, not a mux loop)
#
# A control flop is one whose output reaches, through logic only, a control
# input: the enable, sync reset or async reset of a flop, the select of a mux
# or case, or the enable of a memory port. One with no reset ($dff, $dffe)
# starts at X in a 4-state simulator, and an X on a control input can lock
# the design up (eduBOS5 dmem_vld gated its own enable). A declaration
# initializer is no reset, and neither is a sync clear from logic: a sync
# reset counts when a net named as a reset (rst, reset, arst_n, ...) drives it.
# Data and address flops that end up in a control
# input anyway (a compare of an address, a header field) are waived by name,
# "reset <regex>" lines in run-lint-waivers.txt (an inline /*LINT:OK*/ cannot
# waive these: the name is a hierarchical path). The name reported is the one
# declared in the flop's own file, so a waiver does not depend on port aliases.
#
# Output: one line per flop, "<file>:<line>: '<signal>' ...".

import json, re, sys
from collections import defaultdict

mod = next(m for m in json.load(open(sys.argv[1]))['modules'].values()
           if m.get('attributes', {}).get('top'))

NORESET = {'$dff', '$dffe'}
CONTROL = {'EN', 'SRST', 'ARST', 'ALOAD', 'SET', 'CLR', 'S', 'RD_EN', 'WR_EN', 'RD_ARST', 'RD_SRST'}
STOP    = re.compile(r'^\$(mem|print|check|assert|assume|cover|scopeinfo)|dff|dlatch|sr$')
SINK    = re.compile(r'^\$(mem|mux|pmux|bmux|demux)|dff|dlatch|sr$')  # not $print, $check ...

def bits(c, p):
    return [b for b in c['connections'].get(p, []) if isinstance(b, int)]

def srcfile(o):
    return o.get('attributes', {}).get('src', '').split(':')[0]

nets, aliases = defaultdict(list), defaultdict(list)  # bit -> its (name, file)s, its short names
for nm, net in mod['netnames'].items():
    if not net.get('hide_name'):
        for b in net['bits']:
            nets[b].append((nm, srcfile(net)))
            aliases[b].append(nm.split('.')[-1])

def name_of(b, c):
    """the bit's name declared in the flop's own file, else its deepest one"""
    cand = nets.get(b, [])
    own = [n for n, f in cand if f and f == srcfile(c)]
    pool = own or [n for n, _ in cand]
    return max(pool, key=lambda n: (n.count('.'), len(n))) if pool else None

driver = {}                     # bit -> input bits of the logic cell that drives it
for c in mod['cells'].values():
    t, dirs = c['type'], c.get('port_directions', {})
    if t.startswith('$') and not STOP.search(t):
        ins = [b for p, d in dirs.items() if d == 'input' for b in bits(c, p)]
        for p, d in dirs.items():
            if d == 'output':
                for o in bits(c, p):
                    driver[o] = ins
# the netlist's own connections (assign a = b) are already merged into bits

# opt_dff turns "q <= 0; if (c) q <= 1;" into a sync reset on c: a sync reset
# counts only when a net named as a reset drives it
# a reset net: rst, reset, arst_n, perst_n, s_rst_sync3_reg, rx_rst_r2 ... (not a
# functional clear like rst_idle or reset_req)
RESET_NAME = re.compile(r'(^|_)(a?s?rst|reset|perst)(_?n)?(_sync\d*(_reg)?|_r\d*)?$', re.I)
def is_reset(c):
    seen, work = set(), bits(c, 'SRST')
    while work:
        b = work.pop()
        if b in seen:
            continue
        seen.add(b)
        if any(RESET_NAME.search(n) for n in aliases.get(b, ())):
            return True
        work += driver.get(b, [])
    return False

flop_q = {}                     # Q bit -> flop cell, for flops with no reset
sinks  = []                     # (control bit, sink cell) pairs
for name, c in mod['cells'].items():
    t, dirs = c['type'], c.get('port_directions', {})
    if not t.startswith('$'):
        continue                                    # primitives: their pins are theirs
    for p in c['connections']:
        if p in CONTROL and dirs.get(p, 'input') == 'input' and SINK.search(t):
            sinks += [(b, c) for b in bits(c, p)]
    if t in NORESET or t.startswith('$sdff') and not is_reset(c):
        for q in bits(c, 'Q'):
            flop_q[q] = (name, c)

# backward through the logic from every control input, to the flops
hit = {}                        # flop cell name -> a control sink it reaches
for b0, sink in sinks:
    seen, work = set(), [b0]
    while work:
        b = work.pop()
        if b in seen:
            continue
        seen.add(b)
        if b in flop_q:
            hit.setdefault(flop_q[b][0], sink)
            continue
        work += driver.get(b, [])

def loc(c):
    src = c.get('attributes', {}).get('src', '?').split('|')[0]
    m = re.match(r'(.*?):(\d+)', src)
    return f'{m.group(1)}:{m.group(2)}' if m else src

out = {}                        # one line per signal (a struct is split into cells)
for name, sink in hit.items():
    c = mod['cells'][name]
    sig = name_of(bits(c, 'Q')[0], c) or name
    out.setdefault((loc(c), sig), f"{loc(c)}: '{sig}' has no reset and drives a control input "
                                  f"({sink['type']} at {loc(sink)})")
for k in sorted(out):
    print(out[k])

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# -----------------------------------------------------------------------------
