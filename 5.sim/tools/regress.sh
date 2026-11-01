#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# make regress: the RC model's tester over seeds 1 to SEEDS, JOBS at a time.
# Each seed runs in its own directory, DIR/<seed>/, with copies of the files
# the simulator reads from its working directory (FILES), and with the packet
# display off when QUIET is 1. A seed whose log has no result (the wall-clock
# limit, LIMIT seconds, or a crash) runs again, up to TRIES times; the test
# bench's own time limit does not, it would hang again. Prints one line per
# seed, then the checker's rules no seed checked; exits 1 if any seed failed,
# each with a file DIR/<seed>/failed (make regress REGRESS_DUMP=fail).
#
# Usage: regress.sh SEEDS JOBS LIMIT TRIES QUIET DIR "FILES" -- simulator command...

set -u
seeds=$1 jobs=$2 limit=$3 tries=$4 quiet=$5 dir=$6 files=$7
shift 8
here=$PWD

rm -rf "$dir"
mkdir -p "$dir"

one() {
    local s=$1 d=$dir/$1 t
    shift
    mkdir -p "$d/hex"
    for f in $files; do
        if [ -e "$here/$f" ]; then
            cp -r "$here/$f" "$d/"
        fi
    done
    if [ "$quiet" = 1 ]; then
        printf '    000 000000000000\n    002 009999999999\n' > "$d/hex/ContDisps.hex"
    else
        cp "$here/hex/ContDisps.hex" "$d/hex/"
    fi
    for t in $(seq 1 "$tries"); do
        (cd "$d" && A7RC_SEED=$s timeout -k 30 "$limit" "$@" > regress.log 2>&1)
        echo "$t" > "$d/tries"
        grep -a -q -E 'RC MODEL RESULT|time limit of' "$d/regress.log" && break
    done
}
export -f one
export dir files here quiet tries limit

seq 1 "$seeds" | xargs -P "$jobs" -I{} bash -c 'one "$@"' _ {} "$@"

fails=0
for s in $(seq 1 "$seeds"); do
    r=$(grep -a -m1 'RC MODEL RESULT' "$dir/$s/regress.log" 2>/dev/null)
    # a PASS with a Fatal: or Error: line beside it is a failure
    bad=$(grep -a -c -E '^(Fatal|Error):' "$dir/$s/regress.log" 2>/dev/null)
    t=$(cat "$dir/$s/tries" 2>/dev/null)
    echo "seed $s: ${r:-no result}$([ "${bad:-0}" != 0 ] && echo ", $bad Fatal/Error line(s)")$([ "${t:-1}" != 1 ] && echo ", try $t") ($dir/$s/regress.log)"
    { echo "$r" | grep -q PASS && [ "${bad:-0}" = 0 ]; } || { fails=$((fails + 1)); touch "$dir/$s/failed"; }
done
# Rule coverage: the checks column of each seed's rule table, summed
awk '$1 == "===>" && $2 ~ /^[A-Z]+\.[A-Z0-9-]+$/ && $4 ~ /^[0-9]+$/ { c[$2] += $4 }
     END { n = 0; for (r in c) if (c[r] == 0) { z = z " " r; n++ }
           if (length(c)) printf "%d of %d rules checked in no seed:%s\n", n, length(c), n ? z : " none" }' \
    "$dir"/*/regress.log
echo "$fails of $seeds seeds failed"
[ $fails = 0 ]
