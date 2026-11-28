#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The iob phase of run-lint.sh. Our rule: every top-level data pin can have
# its flop in the I/O tile (7-series ILOGIC/OLOGIC, Vivado IOB = TRUE; the
# GateMate I/O cell's input and output registers), so its timing is the
# pin's, not the place and route's:
#   input   the pin drives the D of a flop directly, no logic between;
#   output  a flop's Q drives the pin directly, and nothing else.
# Clock pins (into a clock input or a clock buffer) and pins on an I/O, clock
# or transceiver primitive (ODDR, IBUFDS, IOBUF, STARTUPE2, IBUFDS_GTE2,
# GTPE2_CHANNEL, CC_SERDES, CC_PLL, ...) are skipped: those are in the I/O
# tile already, or are not data. A slow pin that needs no IOB flop (an LED, a
# reset) is waived by name in run-lint-waivers.txt.
# The same file in openPCIE, openCologne-PCIE and wireguard-fpga.
#
# usage:  python3 run-lint-iob.py <netlist.json>
#   the netlist: Yosys write_json after proc; flatten; opt_expr; opt_clean;
#   opt_dff (the reset phase's), with the vendor primitives as black boxes
#
# Output: one line per pin bit that cannot be an IOB flop,
#   "<file>:<line>: '<port>[<bit>]' <direction>: <reason>"
# with the source line of the cell that is in the way (or of top).

import json
import sys

FF = {'$dff', '$dffe', '$adff', '$adffe', '$sdff', '$sdffe', '$sdffce',
      '$aldff', '$aldffe', '$dffsr', '$dffsre'}
# I/O, transceiver and configuration primitives: a pin on one is not a fabric flop
IO_PRIM = {'ODDR', 'IDDR', 'IDDR_2CLK', 'IBUFDS', 'IBUFGDS', 'OBUFDS', 'OBUFTDS', 'IOBUF',
           'IOBUFDS', 'IBUF', 'IBUFG', 'OBUF', 'OBUFT', 'STARTUPE2', 'ISERDESE2', 'OSERDESE2',
           'IDELAYE2', 'ODELAYE2', 'IBUFDS_GTE2', 'GTPE2_CHANNEL', 'GTPE2_COMMON',
           'GTXE2_CHANNEL', 'GTXE2_COMMON', 'PCIE_2_1',
           # GateMate (Yosys gatemate/cells_bb.v)
           'CC_IBUF', 'CC_OBUF', 'CC_TOBUF', 'CC_IOBUF', 'CC_LVDS_IBUF', 'CC_LVDS_OBUF',
           'CC_LVDS_TOBUF', 'CC_LVDS_IOBUF', 'CC_IDDR', 'CC_ODDR', 'CC_SERDES', 'CC_USR_RSTN',
           'CC_CFG_CTRL'}
CLK_PRIM = {'BUFG', 'BUFGCE', 'BUFGMUX', 'BUFGCTRL', 'BUFH', 'BUFHCE', 'BUFIO', 'BUFR', 'BUFMR',
            'PLLE2_BASE', 'PLLE2_ADV', 'MMCME2_BASE', 'MMCME2_ADV',
            'CC_BUFG', 'CC_PLL', 'CC_PLL_ADV'}


def src(attrs):
  s = attrs.get('src', '')
  s = s.split('|')[0]
  if ':' in s:
    f, pos = s.rsplit(':', 1)
    return f + ':' + pos.split('.')[0]
  return s


def main(path):
  nl = json.load(open(path))
  tops = [m for m, d in nl['modules'].items() if d.get('attributes', {}).get('top')]
  if len(tops) != 1:
    print('error: need one top module, found %d' % len(tops), file=sys.stderr)
    return 2
  top = nl['modules'][tops[0]]
  top_src = src(top.get('attributes', {}))
  # bit -> [(cell, port)] it drives / is driven by
  sinks, driver = {}, {}
  for cname, c in top['cells'].items():
    dirs = c.get('port_directions', {})
    for p, bits in c['connections'].items():
      d = dirs.get(p, 'input')
      for b in bits:
        if isinstance(b, str):
          continue
        if d == 'output':
          driver[b] = (cname, p)
        else:
          sinks.setdefault(b, []).append((cname, p))
  out = []
  for pname, port in top['ports'].items():
    for i, b in enumerate(port['bits']):
      if isinstance(b, str):
        continue  # a constant
      name = "'%s[%d]'" % (pname, i) if len(port['bits']) > 1 else "'%s'" % pname
      if port['direction'] == 'input':
        ss = sinks.get(b, [])
        if not ss:
          continue
        cells = [(top['cells'][c], c, p) for c, p in ss]
        if any(cl['type'] in IO_PRIM or cl['type'] in CLK_PRIM for cl, _, _ in cells):
          continue
        if any(cl['type'] in FF and p in ('CLK', 'C') for cl, _, p in cells):
          continue
        if any(cl['type'] in FF and p == 'D' for cl, _, p in cells):
          continue  # a flop takes it directly: IOB-able
        cl, c, p = cells[0]
        out.append("%s: %s input: no flop takes it directly; first cell %s (%s.%s)"
                   % (src(cl.get('attributes', {})) or top_src, name, cl['type'], c, p))
      elif port['direction'] == 'output':
        if b not in driver:
          continue  # driven by another port or undriven (other checks catch that)
        c, p = driver[b]
        cl = top['cells'][c]
        loc = src(cl.get('attributes', {})) or top_src
        if cl['type'] in IO_PRIM or cl['type'] in CLK_PRIM:
          continue
        if cl['type'] not in FF:
          out.append("%s: %s output: driven by logic (%s %s), not a flop" % (loc, name, cl['type'], c))
          continue
        others = [s for s in sinks.get(b, [])]
        if others:
          out.append("%s: %s output: its flop %s also drives %d cell(s) inside; an IOB flop drives "
                     "the pin only" % (loc, name, c, len(others)))
      else:  # inout: tri-state belongs in an IOBUF; an unconnected one is unused
        cl = [top['cells'][c] for c, _ in sinks.get(b, [])]
        if b in driver:
          cl.append(top['cells'][driver[b][0]])
        if cl and not any(x['type'] in IO_PRIM for x in cl):
          out.append("%s: %s inout: not on an IOBUF" % (top_src, name))
  for line in sorted(set(out)):
    print(line)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1]))
