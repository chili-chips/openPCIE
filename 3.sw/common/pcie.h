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

// The same layout as openCologne-PCIE's: a TLP goes out through tx_payload
// and tx.send, and comes in through the RX ring, whose oldest TLP shows in
// rx_payload. Entry i is DW i of the TLP, header first, both ways.
#define PCIE_TX_SEND         CSR_REG32(tx.send)
#define PCIE_RX_VALID        CSR_REG32(rx.valid)
#define PCIE_ERR_STATUS      CSR_REG32(status.err)
#define PCIE_PHY_STATUS      CSR_REG32(status.phy)
#define PCIE_RESULT          CSR_REG32(status.result)

#define PCIE_RX_PAYLOAD(i)   CSR_REG32(rx_payload.mem[i])
#define PCIE_TX_PAYLOAD(i)   CSR_REG32(tx_payload.mem[i])
#define RX_PAYLOAD_DW        (sizeof(((csr_t *)0)->rx_payload.mem) / sizeof(uint32_t))
#define TX_PAYLOAD_DW        (sizeof(((csr_t *)0)->tx_payload.mem) / sizeof(uint32_t))

// Field masks come from the generated csr.h
#define TX_BUSY_MASK         CSR__STATUS__PHY__TX_BUSY_bm
#define TX_BUF_MASK          CSR__STATUS__PHY__TX_BUF_AVAIL_bm
#define RX_VALID_MASK        CSR__RX__VALID__VALID_bm

// Our Requester ID, bus 0x10, device 29, function 6: no endpoint is given bus 16
#define MY_REQUESTER_ID  0x10EEu

// DW0: Fmt [30:29], Type [28:24], Length [9:0]. Type 0 / Type 1 of a
// Configuration request is Type bit 0: the firmware sets it from the bus
// number (pcie_cfg_write, pcie_cfg_read)
#define TLP_CFG_WR0 0x44000000
#define TLP_CFG_RD0 0x04000000
#define TLP_CFG_T1  0x01000000
#define TLP_MEM_WR  0x40000000
#define TLP_MEM_RD  0x00000000

#define TLP_FMT_DATA(dw0)  (((dw0) >> 30) & 0x1)   // Fmt[1]: with data
#define TLP_TYPE(dw0)      (((dw0) >> 24) & 0x1F)
#define TLP_LENGTH(dw0)    ((dw0) & 0x3FF)         // in DW, 0 = 1024
#define TYPE_CPL           0x0A                    // Cpl, CplD

// Completion header DW1 and DW2
#define CPL_STATUS(dw1)    (((dw1) >> 13) & 0x7)
#define CPL_REQ_ID(dw2)    (((dw2) >> 16) & 0xFFFF)
#define CPL_TAG(dw2)       (((dw2) >> 8) & 0xFF)

// A Memory burst, 1 to 32 DW. A write is bounded by Max_Payload_Size, 128
// bytes, the RC's and the endpoint's default; a read by Max_Read_Request_Size
// (512 bytes by default), so 32 DW is our choice there. Neither may cross a
// 4 KB boundary (PCIe Base 2.2.7)
#define BURST_MAX_DW     32

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
#define CFG_HEADER       0x0C // [22:16] Header Type: 0 endpoint, 1 bridge
#define CFG_CAP_PTR      0x34 // [7:0] first capability
#define CFG_BUS_NUMBERS  0x18 // primary / secondary / subordinate
#define CFG_IO_LIMITS    0x1C // bridge: I/O base [7:0] and limit [15:8]
#define CFG_MEM_LIMITS   0x20 // memory base and limit of the bridge window
#define CFG_PREF_LIMITS  0x24 // bridge: prefetchable base and limit, then
#define CFG_PREF_BASE_HI 0x28 //   their upper halves,
#define CFG_PREF_LIM_HI  0x2C
#define CFG_IO_HI        0x30 //   and those of the I/O window
#define CAP_ID_PCIE      0x10 // PCI Express Capability: Link Capabilities at
#define CAP_LINK_CAP     0x0C //   +0x0C ([20]: DLL Link Active reporting),
#define CAP_LINK_STATUS  0x10 //   Link Status at +0x10 [31:16] ([29]: DLL Link Active)
#define NUM_BARS         6

// Memory Space Enable only. Bus Master stays off: this RC answers no request
// from a device (it would free it unread), so no device may send one
#define CMD_MEM_SPACE    0x00000002

// Stops the firmware: any status other than SC on a checked config access
#define RESULT_CFG_FAILED  0xBAD00002
// The burst self-test (PIO_BURST) read back other data, or no SC
#define RESULT_BURST_FAILED 0xBAD00004
// The Tx side stayed busy, or no transmit buffer came free, for CPL_TIMEOUT_US
#define RESULT_TX_STUCK     0xBAD00005


// Inline, so request() in pcie.c stays a leaf: on eduBOS5 a call costs
// little, the registers it makes the caller save and restore (a 5-cycle load
// each) cost a lot
static inline __attribute__((always_inline)) void wait_cycles(uint32_t n) {  // n from US2WAIT()
#if defined(CPU_COUNTERS) && !defined(VPROC)
    uint32_t t0 = rdcycle();
    while (rdcycle() - t0 < n) {
    }
#elif defined(VPROC)
    // The host build: a loop takes no simulated time, so tick the clock for
    // what a loop takes on eduBOS5, 374 ns, 23 clocks at 62.5 MHz
    VTick(n * 23, SOC_CPU_VPNODE);
#else
    for (uint32_t i = 0; i < n; i++) __asm__("nop");
#endif
}

// Not inlined: each stop is then two instructions
__attribute__((noreturn, noinline)) void halt(uint32_t marker);

// The Tx side idle (tx_payload may change only then) and a free transmit
// buffer in the hard macro. One read of status.phy per check, at most
// US2POLL(CPL_TIMEOUT_US) of them, a hang guard rather than a timer (with
// the cycle counter that bound is cycles, so the wait is longer): a Tx side
// that never frees stops the firmware with RESULT_TX_STUCK
static inline __attribute__((always_inline)) void wait_tx_ready(void) {
    uint32_t polls = US2POLL(CPL_TIMEOUT_US);  // at least 1 for any real timeout
    uint32_t s;

    // Read first, count only while busy: the free case is one read and a test
    while (s = PCIE_PHY_STATUS, ((s & TX_BUSY_MASK) != 0) || ((s & TX_BUF_MASK) == 0)) {
        if (--polls == 0) {
            halt(RESULT_TX_STUCK);
        }
    }
}
// A batch: any number of tx_add() / tx_mem_write(), then tx_go() sends them
// with one tx.send. A full batch goes out by itself; tx_begin() drops one
// not sent yet. Every other send (send_tlp, the pcie_* requests) writes
// tx_payload from entry 0: none between tx_add() and tx_go()
#define TX_BATCH_MAX 255   // tx.send.count is 8 bits
void tx_begin(void);
void tx_add(uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n);
void tx_mem_write(uint32_t addr, const uint32_t *data, uint32_t n);
void tx_go(void);
void send_tlp(uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n);
uint32_t pcie_request(uint32_t type, uint32_t addr_or_id, uint32_t wdata, uint32_t *rdata);
uint32_t pcie_cfg_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t val);
uint32_t pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t *val);
void pcie_mem_write(uint32_t addr, uint32_t val);
uint32_t pcie_mem_read(uint32_t addr, uint32_t *val);
// Bursts of n DW (1 .. BURST_MAX_DW, else nothing is sent: a write is
// dropped, a read returns CPL_TIMEOUT), within one 4 KB page. Memory data is
// not swapped: a DWORD goes out in TLP byte order, byte 0 in bits [31:24]
// (only the cfg helpers swap, see bswap32)
void pcie_mem_write_burst(uint32_t addr, const uint32_t *data, uint32_t n);
uint32_t pcie_mem_read_burst(uint32_t addr, uint32_t *data, uint32_t n);
#ifdef PIO_BURST
int pcie_burst_test(uint32_t addr);  // 0: the burst came back as written

// The throughput test, in BAR0: the 1-DW phase at PERF_SINGLE, the burst at
// PERF_BURST, and the report TLP at PERF_REPORT: PERF_MAGIC | DWORDs moved,
// then the cycles of 1-DW writes, the burst write, 1-DW reads, the burst
// read, CPU_KHZ (0: not measured), and the 1-DW writes as one batch
#define PERF_SINGLE     0x100
#define PERF_BURST      0x200
#define PERF_REPORT     0x300
#define PERF_REPORT_DW  7
#define PERF_MAGIC      0x9E5F0000u
int pcie_perf_test(uint32_t base);   // 0: every DWORD came back as written
#endif
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
