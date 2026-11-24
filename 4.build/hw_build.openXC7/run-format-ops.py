#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# run-format.sh step 4.
# && and || only in a Boolean condition: if (...), else if, while (...),
# wait (...) and the condition of for (...); elsewhere (assign, an assignment,
# return, ?:) & and |. Left alone: SVA (property ... endproperty, sequence ...
# endsequence, assert/assume/cover statements), case (...) expressions and
# `define bodies.
#   usage: run-format-ops.py --report file...   list each && / || outside a condition
#          run-format-ops.py --fix file...      replace them with & / |
# Only safe on 1-bit operands: a multi-bit one changes the meaning, so prove a
# --fix with an equivalence check (and Verilator WIDTH lint).
import re, sys


def blank_comments(s):
    """the same text with comments and strings blanked, so offsets still match"""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', blank, s, flags=re.S)


def paren_spans(code, keywords):
    out = []
    for m in re.finditer(r'\b(%s)\s*\(' % keywords, code):
        i, d = m.end(), 1
        while i < len(code) and d:
            d += {'(': 1, ')': -1}.get(code[i], 0)
            i += 1
        out.append((m.end(), i))
    return out


def skip_spans(code):
    out = paren_spans(code, 'if|while|for|wait|case|casez|casex|iff')
    for m in re.finditer(r'@\s*\(', code):  # an event control: @(posedge clk iff a && b)
        i, d = m.end(), 1
        while i < len(code) and d:
            d += {'(': 1, ')': -1}.get(code[i], 0)
            i += 1
        out.append((m.end(), i))
    for kw in ('property', 'sequence'):
        for m in re.finditer(r'\b%s\b' % kw, code):
            e = re.compile(r'\bend%s\b' % kw).search(code, m.end())
            out.append((m.start(), e.end() if e else len(code)))
    for m in re.finditer(r'\b(assert|assume|cover)\b', code):
        out.append((m.start(), code.find(';', m.start()) + 1 or len(code)))
    for m in re.finditer(r'`define\b', code):
        e = m.start()
        while True:  # to the end of the line, across \ continuations
            e = code.find('\n', e)
            if e < 0 or code[e - 1] != '\\':
                break
            e += 1
        out.append((m.start(), len(code) if e < 0 else e))
    return out


def process(s):
    code = blank_comments(s)
    skip = skip_spans(code)
    return [m.start() for m in re.finditer(r'&&|\|\|', code)
            if not any(a <= m.start() < b for a, b in skip)]


mode = sys.argv[1]
total = 0
for f in sys.argv[2:]:
    s = open(f).read()
    hits = process(s)
    total += len(hits)
    if mode == '--report':
        for p in hits:
            print('&& or || outside if (): %s:%d' % (f, s.count('\n', 0, p) + 1))
    elif hits:
        t = list(s)
        for p in reversed(hits):
            t[p:p + 2] = [s[p]]  # && -> &, || -> |
        open(f, 'w').write(''.join(t))
        print('%d %s' % (len(hits), f))
if mode == '--fix':
    print('%d replaced' % total)

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------
