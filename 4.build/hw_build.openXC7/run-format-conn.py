#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# run-format.sh step 2c.
# At least one space before the '(' of a named parameter or port connection,
# after verible-verilog-format: Verible aligns the '(' of a block of
# connections, flush against the longest name:
#     .ALIGN_COMMA_ENABLE(ALIGN_COMMA_ENABLE),    ->    .ALIGN_COMMA_ENABLE (ALIGN_COMMA_ENABLE),
#     .RX_EN             (rx_en),                 ->    .RX_EN              (rx_en),
# A block (adjacent lines with the '(' in one column) moves as a whole, so it
# stays aligned, with the comment lines that continue a connection's comment.
# Connections that share a line, .a(a), .b(b), get the space too.
#   usage: run-format-conn.py [--check] file...   (--check: print the files it would change)
import re, sys

CONN = re.compile(r'^(\s*\.\w+)(\s*)\(')          # a connection at the start of a line
INLINE = re.compile(r'([(,]\s*\.\w+)\(')          # one after a '(' or a ','
COMMENT = re.compile(r'^\s*(//.*)?$')              # a blank or comment line: does not end a block


def code_end(line):
    i = line.find('//')
    return len(line) if i < 0 else i


def process(text):
    src = text.split('\n')
    col = [m.end() - 1 if (m := CONN.match(l)) else None for l in src]
    i = 0
    while i < len(src):
        if col[i] is None:
            i += 1
            continue
        block, j = [i], i + 1                      # adjacent lines with the '(' in the same column
        while j < len(src) and (col[j] == col[i] or (col[j] is None and COMMENT.match(src[j]) and src[j].strip())):
            if col[j] is not None:
                block.append(j)
            j += 1
        if any(not CONN.match(src[k]).group(2) for k in block):
            for k in block:
                c = src[k].find('//')              # its comment, when it goes on in the next lines
                src[k] = src[k][:col[k]] + ' ' + src[k][col[k]:]
                n = k + 1
                while c >= 0 and n < len(src) and col[n] is None and src[n].find('//') == c and not src[n][:c].strip():
                    src[n] = ' ' + src[n]
                    n += 1
        i = block[-1] + 1
    for i, l in enumerate(src):
        e = code_end(l)
        src[i] = INLINE.sub(r'\1 (', l[:e]) + l[e:]
    return '\n'.join(src)


check = sys.argv[1:2] == ['--check']
for f in sys.argv[1 + check:]:
    t = open(f).read()
    n = process(t)
    if n != t:
        if check:
            print(f)
        else:
            open(f, 'w').write(n)
