// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// soc_rx: the SOC's Rx ring, for tb_soc (bash 5.sim/soc/run-soc.sh rx).
// tb_soc sends RX_TLPS TLPs back to back, more than the ring holds; each
// one goes back as it came, in TX batches of up to BATCH_MAX TLPs, one
// tx.send each. tb_soc checks they come out as they went in.
// From openCologne-PCIE's 3.sw/tests/soc_rx.
//==========================================================================

#include <stdint.h>
#include "pcie.h"

#define RX_TLPS    96  // tb_soc: RX_TLPS
#define BATCH_MAX  16  // TLPs per tx.send

static uint32_t pos;    // the next free tx_payload entry
static uint32_t tlps;   // TLPs in the batch

static void flush(void) {
    PCIE_TX_SEND = tlps;
    wait_tx_ready();
    pos  = 0;
    tlps = 0;
}

int main(void) {
    for (uint32_t t = 0; t < RX_TLPS; t++) {
        while ((PCIE_RX_VALID & RX_VALID_MASK) == 0);

        // Fmt[0]: 4-DW header; Fmt[1]: Length DWORDs of data
        uint32_t dw0 = PCIE_RX_PAYLOAD(0);
        uint32_t dws = ((dw0 & (1u << 29)) ? 4 : 3) + (TLP_FMT_DATA(dw0) ? TLP_LENGTH(dw0) : 0);

        if (tlps == BATCH_MAX || pos + dws > TX_PAYLOAD_DW) {
            flush();
        }
        PCIE_TX_PAYLOAD(pos) = dw0;
        for (uint32_t i = 1; i < dws; i++) {
            PCIE_TX_PAYLOAD(pos + i) = PCIE_RX_PAYLOAD(i);
        }
        pos += dws;
        tlps++;
        PCIE_RX_VALID = RX_VALID_MASK;  // free the head
    }
    flush();
    halt(0x0000FACEu);
}

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 27, 2026
// -----------------------------------------------------------------------------
