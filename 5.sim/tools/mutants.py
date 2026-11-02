#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
"""make mutants: the checker against broken endpoints.

Each mutant breaks the pcieVHost endpoint (or the LTSSM code it shares with
the RC model) in one way, in its own copy of the repo under DIR/<name>/,
runs make run RC=model there, and must fail the rule it breaks. JOBS run side
by side. Prints one line per mutant; exits 1 if a mutant passes, misses its
rule, or gives no result.

Usage: mutants.py JOBS DIR [make arguments...]
"""

import os
import re
import signal
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repo
EXCLUDES = ['.git', 'obj', 'xsim.dir', 'build_artifacts', '*.fst', '*.vcd']
TIMEOUT_S = 3600

# name, the rule it must fail, extra make arguments, edits: (file under
# 5.sim, text, replacement); the text must occur once
MUTANTS = [
    ('pl-skp', 'PL.SKP', [], [
        ('usercode/VUserMain1.cpp', '    InitLinkGen(lanes,',
         '    pcie->configurePcie(CONFIG_DISABLE_SKIPS);   // MUTANT: no SKP\n    InitLinkGen(lanes,')]),
    ('pl-ts-fields', 'PL.TS-FIELDS', [], [
        ('usercode/VUserMain1.cpp', '    InitLinkGen(lanes,',
         '    ConfigurePcieLtssm(CONFIG_LTSSM_TS_CTL, 0x10, node);   // MUTANT: reserved bit\n    InitLinkGen(lanes,')]),
    ('pl-eios-count', 'PL.EIOS', [], [
        ('models/pcievhost/ltssm/ltssm.c', '    for (int i = 0; i < ltssm_cur_gen[node]; i++)',
         '    for (int i = 0; i < 1; i++)   // MUTANT: one EIOS at any rate')]),
    ('pl-eios-idle', 'PL.EIOS', [], [
        ('models/pcievhost/ltssm/ltssm.c', '                SendIdle(idle_us * ticks_us, node);',
         '                SendIdle(100, node);   // MUTANT: idle too short')]),
    ('pl-eios-lock', 'PL.EIOS', [], [
        ('models/pcievhost/ltssm/ltssm.c',
         'timeout  = RECOVERY_RCVRLOCK_TO_US * SYMCLK_TICKS_PER_US(ltssm_cur_gen[node]) / TS_TICKS;',
         'timeout  = (ltssm_cur_gen[node] == 2) ? 4 : RECOVERY_RCVRLOCK_TO_US * SYMCLK_TICKS_PER_US(1) / TS_TICKS;'
         '   // MUTANT: RcvrLock at 5.0 GT/s gives up long before 24 ms')]),
    ('pl-eios-gen1', 'PL.EIOS', ['SIM_RC_GEN=1', 'SIM_SPEED_TEST=1'], [
        ('models/pcievhost/ltssm/ltssm.c',
         'to_speed = directed && (ltssm_cur_gen[node] > 1 || (ltssm_rate[node] & partner & TS_DATA_RATE_5G));',
         'to_speed = directed;   // MUTANT: Recovery.Speed at 2.5 GT/s without 5.0 GT/s (issue 47)')]),
]


def run(mutant, top, make_args):
    name, rule, extra, edits = mutant
    d = top / name
    d.mkdir(parents=True, exist_ok=True)
    subprocess.run(['rsync', '-a', '--delete', '--delete-excluded'] + [f'--exclude={e}' for e in EXCLUDES] +
                   [f'{ROOT}/', f'{d}/repo/'], check=True)
    sim = d / 'repo' / '5.sim'
    for f, text, new in edits:
        p = sim / f
        s = p.read_text()
        if s.count(text) != 1:
            return f'mutant {name}: cannot apply, "{text.strip()}" found {s.count(text)} times'
        p.write_text(s.replace(text, new))
    log = d / 'mutant.log'
    # a make of its own: none of the calling make's flags or jobserver
    env = {k: v for k, v in os.environ.items() if k not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}
    with open(log, 'w') as fh:
        p = subprocess.Popen(['make', 'run', 'RC=model'] + extra + make_args, cwd=sim, stdout=fh,
                             stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            p.wait(timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)   # the simulator too
            p.wait()
    s = log.read_text(errors='replace')
    fails = sorted(set(re.findall(r'^\*\*\*\* CHECK (\S+) ', s, re.M)))
    if 'RC MODEL RESULT: FAIL' in s and rule in fails:
        return f'mutant {name}: fails {rule}, as it must ({", ".join(fails)}) ({log})'
    if 'RC MODEL RESULT: PASS' in s:
        return f'mutant {name}: MISSED, the run passes ({log})'
    if 'RC MODEL RESULT' in s:
        return f'mutant {name}: MISSED, fails {", ".join(fails) or "the tester"}, not {rule} ({log})'
    return f'mutant {name}: no result ({log})'


def main():
    jobs, top, make_args = int(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        lines = list(ex.map(lambda m: run(m, top, make_args), MUTANTS))
    bad = 0
    for line in lines:
        print(line)
        bad += 'as it must' not in line
    print(f'{bad} of {len(MUTANTS)} mutants missed')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
