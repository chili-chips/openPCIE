#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# run-format.sh step 2b.
# Operator-first wrap, after verible-verilog-format: an assignment that does not
# fit on one line (Verible wrapped it, or left it over the limit) gets one
# top-level operand per line, each continuation line starting with the
# operator, under the '=' (or '<=') of the first line. When every operand is
# (a OP b) with the same comparison OP, the OPs line up too.
#   usage: run-format-wrap.py [--check] file...   (--check: print the files it would change)
import re, sys

LIMIT = 100
LOGIC = ['||', '&&', '|', '^', '&', '+']   # lowest precedence first; a split only moves line breaks
CMP = re.compile(r'^\((.+?) (==|!=|<=|>=|===|!==) (.+)\)$')
# a plain assignment: [assign] name[.member][select] = ... (no if, for, call)
START = re.compile(r'^(\s*(?:assign\s+)?[A-Za-z_][\w.]*(?:\[[^\]]*\])*\s*)(<=|=)(\s+)(.*)$')
RETURN = re.compile(r'^(\s*return\s+)(.*)$')
KEYWORD = re.compile(r'\b(if|for|while|case|return|begin|end|else)\b')
# a line that does not end a statement: the next one continues it
OPEN_END = re.compile(r'(&&|\|\||[&|^+\-*/<>=?:,(~!])\s*$')


def code_of(line):
    return re.sub(r'//.*', '', line).rstrip()


def top_split(e, op):
    """split e at depth-0 occurrences of op (not part of a longer operator,
    not a unary reduction)"""
    out, d, i, last = [], 0, 0, 0
    while i < len(e):
        c = e[i]
        if c in '([{':
            d += 1
        elif c in ')]}':
            d -= 1
        elif d == 0 and e.startswith(op, i):
            before = e[:i].rstrip()[-1:]
            after = e[i + len(op):i + len(op) + 1]
            prev = e[i - 1:i]
            if (len(op) == 1 and (after in '&|=' or prev in '&|!<>=~^')) or \
               (len(op) == 2 and (after in '&|=' or prev in '&|')) or \
               before in ('', '(', ',', '&', '|', '^', '~', '!', '?', ':', '='):
                i += 1  # part of another operator, or a unary reduction
                continue
            out.append(e[last:i].strip())
            last = i + len(op)
            i = last
            continue
        elif c == '?' and d == 0:
            return None  # a ?: at the top: leave it to Verible
        i += 1
    out.append(e[last:].strip())
    return out if len(out) > 1 else None


def outer_parens(e):
    """True when e is one (...) group from its first to its last character"""
    if not (e.startswith('(') and e.endswith(')')):
        return False
    d = 0
    for i, c in enumerate(e):
        d += {'(': 1, ')': -1}.get(c, 0)
        if d == 0 and i < len(e) - 1:
            return False
    return True


def wrap(lines):
    """lines: one statement as Verible printed it; returns new lines or None"""
    if any('//' in l or '/*' in l for l in lines):
        return None
    m = START.match(lines[0])
    r = RETURN.match(lines[0])
    if m:
        lhs, aop, first = m.group(1), m.group(2), m.group(4)
        if KEYWORD.search(lhs):
            return None
    elif r:
        lhs, aop, first = r.group(1), '', r.group(2)
    else:
        return None
    if any(KEYWORD.search(l) for l in lines[1:]):
        return None
    rhs = ' '.join([first.strip()] + [l.strip() for l in lines[1:]])
    if not rhs.endswith(';'):
        return None
    rhs = re.sub(r'\s+\)', ')', re.sub(r'\(\s+', '(', rhs[:-1].strip()))  # Verible's breaks in a call
    while outer_parens(rhs):
        rhs = rhs[1:-1].strip()  # redundant: the whole right-hand side in ( )
    for op in LOGIC:
        parts = top_split(rhs, op)
        if parts:
            break
    else:
        return None
    parts = [re.sub(r'^([&|^~]+)\s+', r'\1', p) for p in parts]  # a reduction keeps its operand
    cms = [CMP.match(p) for p in parts]
    if all(cms) and len({c.group(2) for c in cms}) == 1:
        w = max(len(c.group(1)) for c in cms)
        parts = ['(%s %s %s)' % (c.group(1).ljust(w), c.group(2), c.group(3)) for c in cms]
    if aop:  # under the '='
        col = len(lhs)
        out = [lhs + aop + ' ' + parts[0]]
    else:    # return: the operands under the first one
        col = len(lhs) - len(op) - 1
        out = [lhs + parts[0]]
    out += [' ' * col + op + ' ' + p for p in parts[1:]]
    out[-1] += ';'
    if any(len(l) > LIMIT for l in out):
        return None
    return out


def process(text):
    src = text.split('\n')
    res, i = [], 0
    while i < len(src):
        l = src[i]
        prev = next((code_of(p) for p in reversed(res) if code_of(p).strip()), '')
        starts = (START.match(l) or RETURN.match(l)) and '//' not in l and not OPEN_END.search(prev)
        if starts and not l.rstrip().endswith(';'):
            j = i
            while j < len(src) and not src[j].rstrip().endswith(';') and j - i < 12:
                j += 1
            if j < len(src) and src[j].rstrip().endswith(';'):
                new = wrap(src[i:j + 1])
                if new:
                    res += new
                    i = j + 1
                    continue
        elif starts and len(l) > LIMIT:
            new = wrap([l])
            if new:
                res += new
                i += 1
                continue
        res.append(l)
        i += 1
    return '\n'.join(res)


check = sys.argv[1:2] == ['--check']
for f in sys.argv[1 + check:]:
    t = open(f).read()
    n = process(t)
    if n != t:
        if check:
            print(f)
        else:
            open(f, 'w').write(n)

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------
