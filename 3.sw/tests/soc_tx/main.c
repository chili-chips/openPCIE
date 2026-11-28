// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// soc_tx: the SOC's Tx path with 3-DW and 4-DW requests, for tb_soc
// (bash 5.sim/soc/run-soc.sh tx). Sends the requests below through
// send_tlp() and tx_payload, then sets the pass marker; tb_soc compares
// every s_axis_tx beat with the one it expects (the same table there).
// From openCologne-PCIE's 3.sw/tests/soc_tx.
//==========================================================================

#include "pcie.h"

// H0..H3 in tx_payload entries 0 to 3; payload from DW3 (3-DW header) or
// DW4 (4-DW header) on: tx_payload entry i is DW i
#define MWR3(len)  (0x40000000u | (len))   // Fmt 10: 3 DW, data
#define MWR4(len)  (0x60000000u | (len))   // Fmt 11: 4 DW, data
#define MRD3(len)  (0x00000000u | (len))   // Fmt 00: 3 DW
#define MRD4(len)  (0x20000000u | (len))   // Fmt 01: 4 DW

int main(void) {
    // 3-DW MWr, 1 DW
    const uint32_t d3[1] = {0xA1B2C3D4u};
    send_tlp(MWR3(1), 0x0000010Fu, 0x00001000u, d3, 1);

    // 4-DW MWr, 1 DW: Address[63:32] in H2, [31:2] in H3, payload in DW4
    const uint32_t d4[2] = {0x00002000u, 0x11223344u};
    send_tlp(MWR4(1), 0x0000020Fu, 0x00000001u, d4, 2);

    // A batch of three, one tx.send: a 4-DW MWr of 2 DW (entries 0 to 5), a
    // 3-DW MRd (6 to 8), then a 4-DW MRd from an odd entry (9 to 12)
    wait_tx_ready();
    PCIE_TX_PAYLOAD(0)  = MWR4(2);
    PCIE_TX_PAYLOAD(1)  = 0x000003FFu;
    PCIE_TX_PAYLOAD(2)  = 0x00000002u;
    PCIE_TX_PAYLOAD(3)  = 0x00003000u;
    PCIE_TX_PAYLOAD(4)  = 0x55667788u;
    PCIE_TX_PAYLOAD(5)  = 0x99AABBCCu;
    PCIE_TX_PAYLOAD(6)  = MRD3(1);
    PCIE_TX_PAYLOAD(7)  = 0x0000050Fu;
    PCIE_TX_PAYLOAD(8)  = 0x00005000u;
    PCIE_TX_PAYLOAD(9)  = MRD4(1);
    PCIE_TX_PAYLOAD(10) = 0x0000040Fu;
    PCIE_TX_PAYLOAD(11) = 0x00000003u;
    PCIE_TX_PAYLOAD(12) = 0x00004000u;
    PCIE_TX_SEND        = 3;
    wait_tx_ready();

    halt(0x0000FACEu);
}

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 27, 2026
// -----------------------------------------------------------------------------
