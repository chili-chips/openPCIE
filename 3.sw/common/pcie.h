// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// openPCIE * NLnet-sponsored open-source implementation
//--------------------------------------------------------------------------
//                   Copyright (C) 2026 Chili.CHIPS
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//              https://opensource.org/license/bsd-3-clause
//--------------------------------------------------------------------------
// Description:
//   PCIe request layer shared by the RC-direct and RC-switched firmware:
//   CSR access, TLP send / Completion wait with status, CRS retry,
//   configuration helpers and BAR sizing. Each main.c adds only its own
//   topology walk.
//==========================================================================

#ifndef OPENPCIE_PCIE_H
#define OPENPCIE_PCIE_H

#include <stdint.h>

// -------------------------------------------------------------------------
// CSR access: register offsets and field positions come from the headers
// PeakRDL generates out of 4.build/csr_build/csr.rdl, so hardware and
// software cannot drift apart.
// -------------------------------------------------------------------------
#include "csr_regs.h"

#define PCIE_TX_HEADER0      CSR_REG32(tx.header0)
#define PCIE_TX_HEADER1      CSR_REG32(tx.header1)
#define PCIE_TX_HEADER2      CSR_REG32(tx.header2)
#define PCIE_TX_DATA         CSR_REG32(tx.data)

#define PCIE_RX_STATUS       CSR_REG32(rx.status)
#define PCIE_RX_DATA         CSR_REG32(rx.data)
#define PCIE_RX_HEADER_INFO  CSR_REG32(rx.header_info)
#define PCIE_ERR_STATUS      CSR_REG32(status.err)
#define PCIE_PHY_STATUS      CSR_REG32(status.phy)
#define PCIE_RESULT          CSR_REG32(status.result)

// Field masks and positions come from the generated csr.h, via RDL_FIELD()
#define TX_STATE_MASK        CSR__STATUS__PHY__TX_STATE_bm
#define TX_BUF_MASK          CSR__STATUS__PHY__TX_BUF_AVAIL_bm

#define RX_REQUESTER_ID(hdr) RDL_FIELD(hdr, CSR__RX__HEADER_INFO__REQUESTER_ID)
#define RX_TAG(hdr)          RDL_FIELD(hdr, CSR__RX__HEADER_INFO__TAG)


#define MY_REQUESTER_ID  0x10EE

#define TLP_CFG_WR0 0x44000000
#define TLP_CFG_RD0 0x04000000
#define TLP_MEM_WR  0x40000000
#define TLP_MEM_RD  0x00000000

#define CPL_STAT_SC  0 // Successful
#define CPL_STAT_UR  1 // Unsupported Request
#define CPL_STAT_CRS 2 // Configuration Retry Status (Busy)
#define CPL_STAT_CA  4 // Completer Abort
#define CPL_TIMEOUT  8 // not a PCIe status: no Completion arrived in time

// The waits, in us. The simulation builds shorten them (make SIM=1 in
// 4.build/sw_build), so a run ends in time.
//   CPL_TIMEOUT_US  no Completion: give up. 50 ms, the end of a Root Port's
//                   default Completion Timeout range (PCIe Base, 2.8)
//   CRS_WAIT_US     a request answered with CRS is re-issued up to CRS_RETRIES
//                   times, this far apart: 100 x 10 ms, as a device may answer
//                   CRS for up to 1 s after reset (6.6.1)
//   STARTUP_US      idle at start-up, before the first Configuration Request:
//                   100 ms after reset (6.6.1). The SOC leaves reset once the
//                   data link is up (core_rst_n follows trn_link_up), and a
//                   link drop resets it again; CRS covers a slow endpoint
#ifndef CPL_TIMEOUT_US
#define CPL_TIMEOUT_US 50000
#endif
#define CRS_RETRIES    100
#ifndef CRS_WAIT_US
#define CRS_WAIT_US    10000
#endif
#ifndef STARTUP_US
#define STARTUP_US     100000
#endif

// How the waits are timed. CPU_COUNTERS (make COUNTERS=1, the default; the SOC
// then has the CPU's cycle counter): rdcycle, in real time at any -O. CPU_KHZ
// is the CPU clock, the build's PCIe user clock: 62.5 MHz at x1, 125 at x4
// Gen1, 250 at x4 Gen2. Without it (COUNTERS=0, and the host build of
// CPU=vproc): loops, calibrated on eduBOS5 at x1 without -O (a wait_cycles()
// loop 374 ns, a poll 1.09 us), so an -O or a faster clock shortens them.
// The us become cycles or loops at compile time (constants only: rv32i, no
// libgcc multiply).
#if defined(CPU_COUNTERS) && !defined(VPROC)
#ifndef CPU_KHZ
#define CPU_KHZ        62500
#endif
#define US2WAIT(us)    ((uint32_t)((uint64_t)(us) * CPU_KHZ / 1000))   // cycles
#define US2POLL(us)    US2WAIT(us)
static inline uint32_t rdcycle(void) {
    uint32_t c;
    __asm__ volatile("rdcycle %0" : "=r"(c));
    return c;
}
#else
#define US2WAIT(us)    ((uint32_t)((uint64_t)(us) * 1000 / 374))       // loops
#define US2POLL(us)    ((uint32_t)((uint64_t)(us) * 1000 / 1090))      // polls
#endif

// Configuration space offsets
#define CFG_ID           0x00 // Device ID / Vendor ID
#define CFG_COMMAND      0x04
#define CFG_BAR0         0x10
#define CFG_BUS_NUMBERS  0x18 // primary / secondary / subordinate
#define CFG_MEM_LIMITS   0x20 // memory base and limit of the bridge window
#define NUM_BARS         6

#define CMD_MEM_BUS_MASTER 0x00000006 // Memory Space Enable + Bus Master Enable

// Stops the firmware: any status other than SC on a checked config access
#define RESULT_CFG_FAILED  0xBAD00002

extern uint8_t tx_tag;

void wait_cycles(uint32_t n);  // n from US2WAIT()
__attribute__((noreturn)) void halt(uint32_t marker);
void send_tlp(uint32_t h0, uint32_t h1, uint32_t h2, uint32_t data);
uint32_t wait_for_completion(uint8_t tag, uint32_t *data);
uint32_t pcie_request(uint32_t type, uint32_t addr_or_id, uint32_t wdata, uint32_t *rdata);
uint32_t pcie_cfg_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t val);
uint32_t pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t *val);
void pcie_mem_write(uint32_t addr, uint32_t val);
uint32_t pcie_mem_read(uint32_t addr, uint32_t *val);
uint32_t bswap32(uint32_t v);
void cfg_write32(uint32_t bus, uint32_t dev, uint32_t reg, uint32_t val);
uint32_t cfg_read32(uint32_t bus, uint32_t dev, uint32_t reg, uint32_t *val);
uint32_t cfg_read32_checked(uint32_t bus, uint32_t dev, uint32_t reg);
uint32_t assign_bars(uint32_t bus, uint32_t dev, uint32_t win_base, uint32_t win_size);

#endif // OPENPCIE_PCIE_H

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
