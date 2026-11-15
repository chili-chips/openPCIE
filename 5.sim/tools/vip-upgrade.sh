#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chili.CHIPS
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Upgrade the rv32 ISS sources in 5.sim/models/rv32 from upstream
# (github.com/wyvernSemi/rv32) and re-apply our local changes
# (rv32-openpcie.patch). rv32 is used at a commit past its last tag, so pass
# RV32_REF to keep it.
#
# VProc, pcieVHost and mem_model come from openPCIE_BFM (the submodule
# vip/openPCIE_BFM): upgrade them there, with its 4.build/vip-import.sh.
#
# usage:  bash 5.sim/tools/vip-upgrade.sh                    latest tag
#         RV32_REF=774d84fa bash 5.sim/tools/vip-upgrade.sh
#
# Upstream clones go to VIP_WORK (default ~/.cache/openpcie-vip). Needs git,
# sed, patch. Afterwards: make run CPU=iss, update the version table in
# 5.sim/README.md, and review the diff before committing.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
M=$HERE/../models
W=${VIP_WORK:-$HOME/.cache/openpcie-vip}
mkdir -p "$W"

# clone or update a repo, check out $2 (default: its newest tag); prints the ref.
# It runs in $(...), where set -e does not stop the script, so each step
# checks itself and fetch_ref below stops on a failure.
fetch() {
    local repo=$1 ref=${2:-}
    if [ ! -d "$W/$repo" ]; then
        git clone -q "https://github.com/wyvernSemi/$repo.git" "$W/$repo" || return 1
    fi
    git -C "$W/$repo" fetch -q --tags origin || return 1
    git -C "$W/$repo" checkout -q -- . || return 1  # drop a patch from the last run
    [ -n "$ref" ] || ref=$(git -C "$W/$repo" tag --sort=-creatordate | head -1)
    [ -n "$ref" ] || { echo "$repo: no tag found" >&2; return 1; }
    git -C "$W/$repo" checkout -q "$ref" || return 1
    echo "$ref"
}
fetch_ref() { fetch "$@" || { echo "vip-upgrade: fetching $1 failed" >&2; exit 1; }; }

lf() { tr -d '\r' < "$1"; }  # upstream mixes CRLF and LF; we keep LF

RV32_REF=$(fetch_ref rv32 "${RV32_REF:-}") || exit 1
echo "rv32 $RV32_REF"

# ---- rv32: library sources (5.sim/Makefile builds the library) -------------
# Only the files in models/rv32/src are refreshed; a file upstream adds to the
# library must be added there and to RV32_CPP in 5.sim/Makefile by hand.
R=$W/rv32/iss
for f in "$M"/rv32/src/*; do
    src="$R/src/$(basename "$f")"
    [ -f "$src" ] && lf "$src" > "$f" || echo "note: $(basename "$f") not in rv32 $RV32_REF, kept"
done
if ! patch -s --no-backup-if-mismatch -p0 -d "$M/rv32/src" < "$M/rv32/rv32-openpcie.patch"; then
    echo "ERROR: rv32-openpcie.patch no longer applies to rv32 $RV32_REF."
    echo "       Port the fixes by hand into models/rv32/src, then regenerate the patch."
    exit 1
fi

echo "Done. Next: make run CPU=iss, update the version table in 5.sim/README.md,"
echo "review the diff."

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------
