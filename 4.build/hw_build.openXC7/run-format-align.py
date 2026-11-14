#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# run-format.sh step 2d.
# A signal declared next to the assign that drives it, after
# verible-verilog-format: in a block of adjacent logic declarations and assign
# statements the names share a column, and so do the '=' of the assigns.
# Verible aligns declarations among themselves and assigns among themselves,
# and starts over where the two meet:
#     logic s_sent;                      ->    logic  s_sent;
#     logic tx_done;                           logic  tx_done;
#     assign tx_done = s_sent | i_done;        assign tx_done  = s_sent | i_done;
#     logic tx2_done;                          logic  tx2_done;
#     assign tx2_done = s_sent2 | i_done2;     assign tx2_done = s_sent2 | i_done2;
# A block: lines of one indentation, each a whole one-line statement; a blank
# line or any other code ends it, a comment line does not. Blocks of only
# declarations or only assigns stay as Verible made them, and so does a block
# that would pass the column limit.
#
# run-format.sh step 2e.
# One alignment for a module's whole port list: the direction, type, dimension,
# name and comment columns are the same in every group of ports. Verible
# aligns each group (ports between blank or comment lines) on its own:
#     input logic i_clk,       // clock       ->    input  logic      i_clk,       // clock
#     input logic i_link_up,   // link up           input  logic      i_link_up,   // link up
#
#     output logic      o_en,  // enable            output logic      o_en,        // enable
#     output dl_state_e o_st   // state             output dl_state_e o_st         // state
# An interface port has no direction: its type starts in the direction column,
# and its name is in the name column of the others:
#     input  logic  clk,     // PCLK
#     dll_if.tl     dll,
#     trn_tx_if.dst trn_tx,  // TLPs from trn_bridge
# A list with a default value, or one that would pass the column limit, stays
# as Verible made it.
#   usage: run-format-align.py [--check] file...   (--check: print the files it would change)
import re, sys

LIMIT = 100
PORT = re.compile(r'^(\s*)(input|output|inout)\b\s*((?:[\w:$.]+\s+)*?)((?:\[[^\]]*\]\s*)*)'
                  r'([A-Za-z_]\w*(?:\s*\[[^\]]*\])*\s*,?)$')
IFPORT = re.compile(r'^(\s*)([A-Za-z_][\w:$.]*)\s+([A-Za-z_]\w*(?:\s*\[[^\]]*\])*\s*,?)$')
NOT_A_TYPE = {'module', 'import', 'parameter', 'localparam', 'input', 'output', 'inout'}
MODULE = re.compile(r'^\s*module\b')
HEADER_END = re.compile(r'^\s*\)\s*;')
DECL = re.compile(r'^(\s*)((?:logic|wire)(?:\s+(?:signed|unsigned))?)\b\s*((?:\[[^\]]*\]\s*)*)([A-Za-z_].*;)$')
ASSIGN = re.compile(r'^(\s*)assign\s+(\S.*?)\s+=\s+(.*;)$')
COMMENT = re.compile(r'^\s*//')


def split(line):
    """the statement and its trailing comment (with the spaces before it)"""
    i = line.find('//')
    if i < 0:
        return line.rstrip(), ''
    code = line[:i].rstrip()
    return code, line[len(code):]


def parse(line):
    code, cmt = split(line)
    m = DECL.match(code)
    if m:
        return ('decl', m.group(1), m.group(2), ' '.join(m.group(3).split()), m.group(4), cmt)
    m = ASSIGN.match(code)
    if m:
        return ('assign', m.group(1), m.group(2), m.group(3), cmt)
    return None


def align(items):
    """items: parsed lines of one block -> their new text"""
    decls = [p for p in items if p[0] == 'decl']
    head = max(len(p[2]) for p in decls)
    dims = max(len(p[3]) for p in decls)
    pre = max(len('assign'), head + (1 + dims if dims else 0))
    lhs = max(len(p[2]) for p in items if p[0] == 'assign')
    out = []
    for p in items:
        if p[0] == 'decl':
            t = p[2].ljust(head) + (' ' + p[3].ljust(dims) if dims else '')
            out.append(p[1] + t.ljust(pre) + ' ' + p[4] + p[5])
        else:
            out.append(p[1] + 'assign'.ljust(pre) + ' ' + p[2].ljust(lhs) + ' = ' + p[3] + p[4])
    return out


def carry(src, k, old, new):
    """line k's comment moved from column old to new: the comment lines that
    continue it (the next lines, a comment alone in that column) follow"""
    n = k + 1
    while old >= 0 and new >= 0 and n < len(src) and src[n].find('//') == old and not src[n][:old].strip():
        src[n] = ' ' * new + src[n][old:]
        n += 1


def process(text):
    src = text.split('\n')
    i = 0
    while i < len(src):
        first = parse(src[i])
        if not first:
            i += 1
            continue
        idx, items, j = [i], [first], i + 1
        while j < len(src):
            p = parse(src[j])
            if p and p[1] == first[1]:
                idx.append(j)
                items.append(p)
            elif not COMMENT.match(src[j]):
                break
            j += 1
        if {p[0] for p in items} == {'decl', 'assign'}:
            new = align(items)
            if all(len(l) <= LIMIT or l == src[k] for k, l in zip(idx, new)):
                for k, l in reversed(list(zip(idx, new))):
                    old = src[k].find('//')
                    src[k] = l
                    carry(src, k, old, l.find('//'))
        i = idx[-1] + 1
    return '\n'.join(ports(src))


def ports(src):
    """step 2e: every module header's port list, as one alignment"""
    i = 0
    while i < len(src):
        if not MODULE.match(src[i]):
            i += 1
            continue
        j = i
        while j < len(src) and not HEADER_END.match(src[j]) and not (j > i and MODULE.match(src[j])):
            j += 1
        rows = []  # (line, indent, direction, type, dimensions, name, comment)
        for k in range(i, j):
            code, cmt = split(src[k])
            m = PORT.match(code)
            n = IFPORT.match(code)
            if m:
                rows.append((k, m.group(1), m.group(2), ' '.join(m.group(3).split()), m.group(4).rstrip(),
                             m.group(5), cmt.strip()))
            elif n and n.group(2) not in NOT_A_TYPE:  # an interface port: no direction
                rows.append((k, None, '', n.group(2), '', n.group(3), cmt.strip()))
            elif re.match(r'^\s*(input|output|inout)\b', code):
                rows = []  # a port this does not parse (a default value): leave the list alone
                break
        ind = {r[1] for r in rows if r[1] is not None}
        if len(ind) == 1:
            rows = [(r[0], ind.copy().pop()) + r[2:] for r in rows]
            dirs = [r for r in rows if r[2]]
            wt = max(len(r[3]) for r in dirs)
            wd = max(len(r[4]) for r in dirs)
            pre = []  # all before the name: direction, type and dimensions, or the interface type
            for r in rows:
                if not r[2]:
                    pre.append(r[3])
                    continue
                l = r[2].ljust(6)
                if wt:
                    l += ' ' + r[3].ljust(wt)
                if wd:
                    l += ' ' + r[4].ljust(wd)
                pre.append(l.rstrip())
            wp = max(len(l) for l in pre)
            new = [r[1] + l.ljust(wp) + ' ' + r[5] for l, r in zip(pre, rows)]
            wc = max(len(l) for l in new) + 2
            new = [l.ljust(wc) + r[6] if r[6] else l for l, r in zip(new, rows)]
            if all(len(l) <= LIMIT or l == src[r[0]] for l, r in zip(new, rows)):
                for l, r in reversed(list(zip(new, rows))):
                    old = src[r[0]].find('//')
                    src[r[0]] = l
                    carry(src, r[0], old, l.find('//'))
        i = j + 1
    return src


check = sys.argv[1:2] == ['--check']
for f in sys.argv[1 + check:]:
    t = open(f).read()
    n = process(t)
    if n != t:
        if check:
            print(f)
        else:
            open(f, 'w').write(n)
