#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Regression, before a push: the sanity runs, then the link widths and rates,
# the switched variant, the other CPUs and cores, the endpoint's faults
# (SIM_FAULT) on both RCs, the RC model's PHYs and load, 100 random seeds of
# the tester, the checker against broken endpoints (mutants), and eduBOS5's
# RV32M bench.
# A run that fails by design is reported as "known"; update KNOWN when that
# changes. Prints one line per run, logs in obj/regression/; exits 1 if a run
# fails that should pass, or a known one passes.
# Needs the environment of 4.build/set-env.sh (Vivado for XSIM).
#
# Usage: bash run-regression.sh [make variables, e.g. SEED=2]

set -u
cd "$(dirname "$0")"
HERE=$PWD
LOG=$HERE/obj/regression
mkdir -p $LOG
fails=0

# the runs that fail today, and why
declare -A KNOWN=(
    [hm-fault-nocpl]="by design"      # SIM_FAULT=nocpl: the request is never completed
    [model-fault-nocpl]="by design"
)
# ... and what the log of each must show: another failure is not the known one
declare -A KNOWN_SIG=(
    [hm-fault-nocpl]="0xbad00002"     # the firmware's no-completion code
    [model-fault-nocpl]="TL.CPL-ALL"
)

report() {  # name, seconds, pass (0/1), what, result
    local n=$1 t=$2 ok=$3 what=$4 r=$5 st
    if [ -n "${KNOWN[$n]:-}" ]; then
        if [ $ok = 1 ]; then st="PASS, known ${KNOWN[$n]}: update KNOWN"; fails=$((fails + 1))
        elif ! grep -a -q -i -F "${KNOWN_SIG[$n]}" $LOG/$n.log; then
            st="FAIL, not as known ${KNOWN[$n]}: no \"${KNOWN_SIG[$n]}\""; fails=$((fails + 1))
        else st="known: ${KNOWN[$n]}"; fi
    elif [ $ok = 1 ]; then st="PASS"
    else st="FAIL"; fails=$((fails + 1)); fi
    printf '%-20s %5d s  %-46s %s  (%s)\n' "$n" "$t" "$what" "$st" "$r"
}

run() {  # name, make variables
    local n=$1 t0=$SECONDS r ok=0
    shift
    make run "$@" > $LOG/$n.log 2>&1 && ok=1
    r=$(grep -a -m1 'FIRMWARE RESULT\|RC MODEL RESULT\|time limit\|^Fatal:' $LOG/$n.log | sed 's/^.*===> *//')
    case "$r" in *"PASS"*) ;; *) ok=0 ;; esac
    report $n $((SECONDS - t0)) $ok "make $*" "${r:-no result}"
}

# The hard macro + firmware (RC=hm, the default)
run hm                  "$@"
run hm-gen1             SIM_GEN=1 "$@"
run hm-speed            SIM_SPEED_TEST=1 "$@"
run hm-x4               SIM_LANES=4 "$@"
run hm-x4g2             SIM_LANES=4 SIM_GEN=2 "$@"
run hm-switched         VARIANT=switched "$@"
run hm-vproc            CPU=vproc "$@"
run hm-iss              CPU=iss "$@"
run hm-picorv32         CORE=picorv32 "$@"
run hm-fault-crs        SIM_FAULT=crs RUN_US=2000 "$@"
run hm-fault-late       SIM_FAULT=late "$@"
run hm-fault-nocpl      SIM_FAULT=nocpl RUN_US=3000 "$@"

# The Artix-7 RC model (RC=model)
run model               RC=model "$@"
run model-janus         RC=model SIM=janushdl "$@"
if diff -q <(grep -a '===>' $LOG/model.log) <(grep -a '===>' $LOG/model-janus.log) > /dev/null; then
    report sim-match 0 1 "XSIM = JanusHDL, RC=model" "the same ===> lines"
else
    report sim-match 0 0 "XSIM = JanusHDL, RC=model" "diff the ===> lines of model.log and model-janus.log"
fi
run model-gen1          RC=model SIM_GEN=1 "$@"
run model-speed         RC=model SIM_SPEED_TEST=1 "$@"
run model-x4            RC=model SIM_LANES=4 "$@"
run model-serial        RC=model IFACE=serial "$@"
run model-phy-core      RC=model PHY=core "$@"
run model-phy-hm        RC=model PHY=hm "$@"
run model-literal       RC=model A7RC=1 "$@"
run model-load          RC=model MIX=1000 "$@"
run model-fault-crs     RC=model SIM_FAULT=crs "$@"
run model-fault-late    RC=model SIM_FAULT=late "$@"
run model-fault-nocpl   RC=model SIM_FAULT=nocpl RUN_US=3000 "$@"

# 100 seeds of the tester
t0=$SECONDS
make regress RC=model SEEDS=100 REGRESS_JOBS=8 "$@" > $LOG/regress.log 2>&1
r=$(grep -a 'seeds failed' $LOG/regress.log | tail -1)
case "$r" in "0 of "*) ok=1;; *) ok=0;; esac
report regress $((SECONDS - t0)) $ok "make regress RC=model SEEDS=100" "${r:-no result}; $(grep -a 'rules checked in no seed' $LOG/regress.log | tail -1)"

# The checker against broken endpoints (openPCIE_BFM 4.build/mutants.py)
t0=$SECONDS
make mutants MUTANT_JOBS=6 > $LOG/mutants.log 2>&1 && ok=1 || ok=0
report mutants $((SECONDS - t0)) $ok "make mutants" "$(grep -a 'mutants missed' $LOG/mutants.log)"

# eduBOS5's RV32M bench: every configuration PASS, none FAIL
t0=$SECONDS
bash ../2.rtl/0.common.opensource/src/CPU.eduBOS5/sim/run-muldiv.sh > $LOG/muldiv.log 2>&1 && ok=1 || ok=0
np=$(grep -a -c '^PASS' $LOG/muldiv.log); nf=$(grep -a -c '^FAIL' $LOG/muldiv.log)
[ "$nf" = 0 ] && [ "$np" -ge 4 ] || ok=0
report muldiv $((SECONDS - t0)) $ok "run-muldiv.sh (eduBOS5 RV32M)" "$np PASS, $nf FAIL"

echo "regression: $fails unexpected"
[ $fails = 0 ]
