#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Sanity, after every change: the hard macro + firmware (RC=hm) and the
# Artix-7 RC model (RC=model) once each way. Each run must end with its PASS
# (FIRMWARE RESULT for RC=hm, RC MODEL RESULT for RC=model) and exit 0; XSIM
# and JanusHDL must print the same ===> lines.
# Prints one line per run, logs in obj/sanity/; exits 1 if any run fails.
# Needs the environment of 4.build/set-env.sh (Vivado for XSIM).
#
# Usage: bash run-sanity.sh [make variables, e.g. SEED=2]

set -u
cd "$(dirname "$0")"
LOG=$PWD/obj/sanity
mkdir -p $LOG
fails=0

run() {  # name, make variables
    local n=$1 t0=$SECONDS r ok=0
    shift
    make run "$@" > $LOG/$n.log 2>&1 && ok=1
    r=$(grep -a -m1 'FIRMWARE RESULT\|RC MODEL RESULT\|time limit\|^Fatal:' $LOG/$n.log | sed 's/^.*===> *//')
    case "$r" in *"PASS"*) ;; *) ok=0 ;; esac
    [ $ok = 1 ] || fails=$((fails + 1))
    printf '%-14s %4d s  %-4s %-34s %s\n' "$n" $((SECONDS - t0)) "$([ $ok = 1 ] && echo PASS || echo FAIL)" "$*" "${r:-no result, see $LOG/$n.log}"
}

run hm            "$@"
run hm-vproc      CPU=vproc "$@"
run model         RC=model "$@"
run model-janus   RC=model SIM=janushdl "$@"
run model-serial  RC=model IFACE=serial "$@"
run model-x4      RC=model SIM_LANES=4 "$@"

if diff -q <(grep -a '===>' $LOG/model.log) <(grep -a '===>' $LOG/model-janus.log) > /dev/null; then
    echo "XSIM = JanusHDL: the same ===> lines"
else
    echo "XSIM and JanusHDL differ: diff <(grep -a '===>' $LOG/model.log) <(grep -a '===>' $LOG/model-janus.log)"
    fails=$((fails + 1))
fi
echo "sanity: $fails failed"
[ $fails = 0 ]
