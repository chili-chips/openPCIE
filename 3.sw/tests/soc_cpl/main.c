// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// soc_cpl: the RC request layer (3.sw/common/pcie.c) against tb_soc's
// answers (bash 5.sim/soc/run-soc.sh cpl):
//   1. a 32-DW burst read. tb_soc answers with a Message and a stale
//      Completion (another tag), which wait_for_completion() must free
//      unread, then two 16-DW CplDs, split at the 64-byte Read Completion
//      Boundary: DWORD i must read 0xD0000000 + i
//   2. a 1-DW read, answered with UR: the status must come back
// 0x0000FACE when both hold; 0xDEAD000n says which did not.
//==========================================================================

#include "pcie.h"

int main(void) {
    uint32_t d[BURST_MAX_DW];
    uint32_t v;

    if (pcie_mem_read_burst(0x80000000u, d, BURST_MAX_DW) != CPL_STAT_SC) {
        halt(0xDEAD0001u);
    }
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        if (d[i] != (0xD0000000u | i)) {
            halt(0xDEAD0002u);
        }
    }
    if (pcie_mem_read(0x80000100u, &v) != CPL_STAT_UR) {
        halt(0xDEAD0003u);
    }
    halt(0x0000FACEu);
}

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 27, 2026
// -----------------------------------------------------------------------------
