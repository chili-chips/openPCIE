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
//   PCIe request layer shared by the RC-direct and RC-switched firmware.
//   See pcie.h.
//==========================================================================

#include "pcie.h"

// Counts the requests; a TLP carries its low 5 bits as the tag (TX_TAG): the
// RC does not enable Extended Tag (DEV_CONTROL_EXT_TAG_DEFAULT), so tags are
// 0..31
static uint8_t tx_tag;  // start.S zeroes it
#define TX_TAG() (tx_tag++ & 0x1F)

__attribute__((noreturn, noinline)) void halt(uint32_t marker) {
    PCIE_RESULT = marker;
#ifdef VPROC
    // Co-simulation (make CPU=vproc): park the VProc node without stopping
    // the simulation
    while (1) VTick(10000, SOC_CPU_VPNODE);
#else
    while (1) {}
#endif
}

// A 3-DW header and n payload DWORDs into tx_payload from entry pos. Always
// inlined: on eduBOS5 a call costs about as much as the stores themselves
static inline __attribute__((always_inline)) void
tx_put(uint32_t pos, uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n) {
    PCIE_TX_PAYLOAD(pos)     = h0;
    PCIE_TX_PAYLOAD(pos + 1) = h1;
    PCIE_TX_PAYLOAD(pos + 2) = h2;
    for (uint32_t i = 0; i < n; i++) {
        PCIE_TX_PAYLOAD(pos + 3 + i) = data[i];
    }
}

// One TLP on its own, from entry 0. The header's Fmt and Length tell the Tx
// side how many DWORDs go out
void send_tlp(uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n) {
    wait_tx_ready();
    tx_put(0, h0, h1, h2, data, n);
    PCIE_TX_SEND = 1;
}

// A batch: TLPs back to back in tx_payload from entry 0, then their number
// into tx.send. tx_payload must not change while the Tx side sends, so a
// batch starts only once it is idle. A full core TX buffer just holds the
// Tx side.
static uint32_t tx_pos;   // next free entry
static uint32_t tx_tlps;  // TLPs in the batch

// Drops what tx_add() collected and not sent yet
void tx_begin(void) {
    tx_pos  = 0;
    tx_tlps = 0;
}

// Sends the batch, and leaves the next one empty
void tx_go(void) {
    if (tx_tlps != 0) {
        PCIE_TX_SEND = tx_tlps;
        tx_pos  = 0;
        tx_tlps = 0;
    }
}

// Adds a TLP to the batch. A batch that is full goes first. The first TLP
// of a batch waits until the Tx side has sent the last one. Inlined into
// tx_add() and tx_mem_write(), so each is a leaf
static inline __attribute__((always_inline)) void
tx_add_in(uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n) {
    uint32_t pos = tx_pos;
    uint32_t cnt = tx_tlps;

    if (pos + 3 + n > TX_PAYLOAD_DW || cnt == TX_BATCH_MAX) {
        PCIE_TX_SEND = cnt;  // tx_go(); a count of 0 (n too large) starts nothing
        pos = 0;
        cnt = 0;
    }
    if (cnt == 0) {
        wait_tx_ready();
    }
    tx_put(pos, h0, h1, h2, data, n);
    tx_pos  = pos + 3 + n;
    tx_tlps = cnt + 1;
}

void tx_add(uint32_t h0, uint32_t h1, uint32_t h2, const uint32_t *data, uint32_t n) {
    tx_add_in(h0, h1, h2, data, n);
}

// Writing 1 to rx.valid frees the ring's head: the next TLP takes its place
static void rx_release(void) {
    PCIE_RX_VALID = RX_VALID_MASK;
}

// Waits for the Completions of the request with `tag` and returns their
// Completion Status (CPL_STAT_*), or CPL_TIMEOUT. A read's n DWORDs may come
// in several CplDs (the completer splits at its Read Completion Boundary);
// they arrive in address order (PCIe Base 2.3.1.1), so each one's payload
// follows the last. A payload is stored to data only from an SC Completion,
// so a genuine 0xFFFFFFFF read is not mistaken for an error; a split read
// that fails part-way may leave data partly written. Every other TLP in the RX
// ring -- a stale Completion, a Message -- is freed unread.
static inline __attribute__((always_inline)) uint32_t
wait_for_completion(uint8_t tag, uint32_t *data, uint32_t n) {
    uint32_t got = 0;  // DWORDs stored so far
#if defined(CPU_COUNTERS) && !defined(VPROC)
    uint32_t t0 = rdcycle();

    while (rdcycle() - t0 < US2POLL(CPL_TIMEOUT_US)) {
#else
    volatile uint32_t timeout = US2POLL(CPL_TIMEOUT_US);

    while (timeout > 0) {
        timeout--;
#endif
        if ((PCIE_RX_VALID & RX_VALID_MASK) == 0) {
            continue;
        }
        uint32_t dw0 = PCIE_RX_PAYLOAD(0);
        uint32_t dw2 = PCIE_RX_PAYLOAD(2);

        // DW2 [31:8]: Requester ID and Tag, one compare
        if (TLP_TYPE(dw0) == TYPE_CPL && (dw2 >> 8) == ((MY_REQUESTER_ID << 8) | tag)) {
            uint32_t status = CPL_STATUS(PCIE_RX_PAYLOAD(1));

            if (status != CPL_STAT_SC) {
                rx_release();
                return status;
            }
            if (TLP_FMT_DATA(dw0)) {
                uint32_t len = TLP_LENGTH(dw0);

                for (uint32_t i = 0; i < len && got < n; i++) {
                    data[got++] = PCIE_RX_PAYLOAD(3 + i);
                }
            }
            rx_release();
            if (got >= n) {
                return CPL_STAT_SC;
            }
            continue;  // more CplDs of a split read
        }
        rx_release();  // not ours
    }
    return CPL_TIMEOUT;
}

// Sends one non-posted request, header h0 / byte enables be / h2 (and wdata
// when h0 has data), and waits for its n DWORDs. A device that is still
// initialising may answer a Configuration Request -- read or write -- with
// CRS; the request is then re-issued, with a fresh tag, up to CRS_RETRIES
// times. Returns the final Completion Status.
// One leaf function, everything inlined: no registers to save. The attribute
// keeps -Os from hoisting a dozen constants out of the retry loop into saved
// registers, each a 5-cycle load to restore on eduBOS5
__attribute__((optimize("no-move-loop-invariants"))) static uint32_t
request(uint32_t h0, uint32_t be, uint32_t h2, uint32_t wdata, uint32_t *rdata, uint32_t n) {
    uint32_t status = CPL_TIMEOUT;

    for (int retry_count = 0; retry_count <= CRS_RETRIES; retry_count++) {

        uint8_t current_tag = TX_TAG();

        wait_tx_ready();
        PCIE_TX_PAYLOAD(0) = h0;
        PCIE_TX_PAYLOAD(1) = (MY_REQUESTER_ID << 16) | (current_tag << 8) | be;
        PCIE_TX_PAYLOAD(2) = h2;
        if (TLP_FMT_DATA(h0)) {
            PCIE_TX_PAYLOAD(3) = wdata;
        }
        PCIE_TX_SEND = 1;

        status = wait_for_completion(current_tag, rdata, n);

        // CRS is a Configuration Request's answer only
        if (status != CPL_STAT_CRS || (h0 & TLP_CFG_RD0) == 0) {
            break;
        }
        wait_cycles(US2WAIT(CRS_WAIT_US));
    }
    return status;
}

uint32_t pcie_request(uint32_t type, uint32_t addr_or_id, uint32_t wdata, uint32_t *rdata) {
    return request(type | 0x01, 0x0F, addr_or_id & 0xFFFFFFFC, wdata, rdata, rdata ? 1 : 0);
}

// Configuration requests are routed by bus number, as the AMD packet
// generator does it: bus 1, on the link, gets Type 0; a bus past it (behind
// a switch) gets Type 1, to cross one or more bridges. The device number
// needs no such treatment: it travels in DW2, where the completer expects it
static uint32_t cfg_type(uint32_t type0, uint32_t bus) {
    return (bus > 1) ? (type0 | TLP_CFG_T1) : type0;
}

// DW2: Bus, Device, Function, Extended Register Number [11:8], Register
// Number [7:2]
uint32_t pcie_cfg_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t val) {
    uint32_t id = (bus << 24) | (dev << 19) | (func << 16) | (reg & 0xFFC);
    return pcie_request(cfg_type(TLP_CFG_WR0, bus), id, val, 0);
}

uint32_t pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t *val) {
    uint32_t id = (bus << 24) | (dev << 19) | (func << 16) | (reg & 0xFFC);
    return pcie_request(cfg_type(TLP_CFG_RD0, bus), id, 0, val);
}

// Byte enables of a Memory request of n DW: all of the first; the last DW's
// all, or 0 when there is only one (PCIe Base 2.2.5)
static uint32_t mem_be(uint32_t n) {
    return (n == 1) ? 0x0F : 0xFF;
}

// A Memory Write of n DW added to the batch. Posted: no Completion to wait
// for, so writes batch freely, and their tag means nothing: 0. Tags count
// non-posted requests only
void tx_mem_write(uint32_t addr, const uint32_t *data, uint32_t n) {
    if (n - 1 >= BURST_MAX_DW) {  // 0 would be Length 1024
        return;
    }
    tx_add_in(TLP_MEM_WR | n,
           (MY_REQUESTER_ID << 16) | mem_be(n),
           addr & 0xFFFFFFFC,
           data, n);
}

void pcie_mem_write_burst(uint32_t addr, const uint32_t *data, uint32_t n) {
    if (n - 1 >= BURST_MAX_DW) {  // 0 would be Length 1024
        return;
    }
    send_tlp(TLP_MEM_WR | n,
             (MY_REQUESTER_ID << 16) | mem_be(n),
             addr & 0xFFFFFFFC,
             data, n);
}

uint32_t pcie_mem_read_burst(uint32_t addr, uint32_t *data, uint32_t n) {
    if (n - 1 >= BURST_MAX_DW) {  // 0 would be Length 1024
        return CPL_TIMEOUT;
    }
    return request(TLP_MEM_RD | n, mem_be(n), addr & 0xFFFFFFFC, 0, data, n);
}

#ifdef PIO_BURST
// Burst self-test (make PIO_BURST=1): BURST_MAX_DW DWORDs out in one Memory
// Write, back in one Memory Read. 128 bytes: the endpoint may split the
// read at its Read Completion Boundary, 64 bytes, into two CplDs. Not for
// the AMD PIO endpoint, which answers only 1-DW requests (PIO_RX_ENGINE.v)
int pcie_burst_test(uint32_t addr) {
    uint32_t wr[BURST_MAX_DW];
    uint32_t rd[BURST_MAX_DW];

    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        wr[i] = 0xB0000000u | (i << 8) | i;
        rd[i] = 0;
    }
    pcie_mem_write_burst(addr, wr, BURST_MAX_DW);
    if (pcie_mem_read_burst(addr, rd, BURST_MAX_DW) != CPL_STAT_SC) {
        return -1;
    }
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        if (rd[i] != wr[i]) {
            return -1;
        }
    }
    return 0;
}

// PIO throughput (make PIO_BURST=1): the same BURST_MAX_DW DWORDs, 128 bytes,
// moved as 1-DW requests and as one burst, written then read back; the 1-DW
// writes also go once as a batch. Each phase is timed in CPU cycles, from its
// first CSR access until its last TLP has left (writes) or its last
// Completion is read (reads), so the time includes what the firmware spends:
// loading tx_payload, polling, reading rx_payload. The five counts and
// CPU_KHZ go to the endpoint in a report TLP, a
// Memory Write to base + PERF_REPORT; 5.sim/tb.sv turns it into MB/s. They
// are 0 without the cycle counter (COUNTERS=0, CPU=vproc). Returns 0 when
// every DWORD came back as written.
static uint32_t perf_now(void) {
#if defined(CPU_COUNTERS) && !defined(VPROC)
    return rdcycle();
#else
    return 0;
#endif
}

int pcie_perf_test(uint32_t base) {
    uint32_t buf[BURST_MAX_DW];
    uint32_t rep[PERF_REPORT_DW];
    uint32_t v;
    uint32_t t;
    int      bad = 0;

    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        buf[i] = 0xC0000000u | i;
    }

    // A write phase ends once the Tx side has sent it all, so it does not
    // leave its drain to the next phase
    t = perf_now();
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        pcie_mem_write(base + PERF_SINGLE + (i << 2), 0xA0000000u | i);
    }
    wait_tx_ready();
    rep[1] = perf_now() - t;

    // The same 1-DW writes as one batch, one tx.send for all, with other
    // data: the 1-DW reads below check the batch
    t = perf_now();
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        v = 0xA1000000u | i;
        tx_mem_write(base + PERF_SINGLE + (i << 2), &v, 1);
    }
    tx_go();
    wait_tx_ready();
    rep[6] = perf_now() - t;

    t = perf_now();
    pcie_mem_write_burst(base + PERF_BURST, buf, BURST_MAX_DW);
    wait_tx_ready();
    rep[2] = perf_now() - t;

    t = perf_now();
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        if (pcie_mem_read(base + PERF_SINGLE + (i << 2), &v) != CPL_STAT_SC ||
            v != (0xA1000000u | i)) {
            bad = 1;
        }
    }
    rep[3] = perf_now() - t;

    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        buf[i] = 0;
    }
    t = perf_now();
    if (pcie_mem_read_burst(base + PERF_BURST, buf, BURST_MAX_DW) != CPL_STAT_SC) {
        bad = 1;
    }
    rep[4] = perf_now() - t;
    for (uint32_t i = 0; i < BURST_MAX_DW; i++) {
        if (buf[i] != (0xC0000000u | i)) {
            bad = 1;
        }
    }

    rep[0] = PERF_MAGIC | BURST_MAX_DW;
#if defined(CPU_COUNTERS) && !defined(VPROC)
    rep[5] = CPU_KHZ;
#else
    rep[5] = 0;
#endif
    pcie_mem_write_burst(base + PERF_REPORT, rep, PERF_REPORT_DW);
    return bad ? -1 : 0;
}
#endif

// The 4 CSRs written directly: no val on the stack, no n loop
void pcie_mem_write(uint32_t addr, uint32_t val) {
    wait_tx_ready();
    PCIE_TX_PAYLOAD(0) = TLP_MEM_WR | 1;
    PCIE_TX_PAYLOAD(1) = (MY_REQUESTER_ID << 16) | 0x0F;
    PCIE_TX_PAYLOAD(2) = addr & 0xFFFFFFFC;
    PCIE_TX_PAYLOAD(3) = val;
    PCIE_TX_SEND = 1;
}

uint32_t pcie_mem_read(uint32_t addr, uint32_t *val) {
    return pcie_mem_read_burst(addr, val, 1);
}

//--------------------------------------------------------------------------
// Configuration space payloads travel big-endian: byte 0 of the register ends
// up in bits [31:24] of the data dword. The two helpers below swap on the way
// in and out, so every value in the firmware is written the way the spec prints
// it (BAR 0x80000000, not 0x00000080).
//
// cfg_write32() is the checked write: any status other than SC stops the
// firmware with RESULT_CFG_FAILED.
//--------------------------------------------------------------------------
uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0xFF000000u) >> 24);
}

void cfg_write32(uint32_t bus, uint32_t dev, uint32_t reg, uint32_t val) {
    if (pcie_cfg_write(bus, dev, 0, reg, bswap32(val)) != CPL_STAT_SC) {
        halt(RESULT_CFG_FAILED);
    }
}

uint32_t cfg_read32(uint32_t bus, uint32_t dev, uint32_t reg, uint32_t *val) {
    uint32_t raw;
    uint32_t status = pcie_cfg_read(bus, dev, 0, reg, &raw);

    if (status == CPL_STAT_SC) {
        *val = bswap32(raw);
    }
    return status;
}

// The checked read, for a device already known to be there
uint32_t cfg_read32_checked(uint32_t bus, uint32_t dev, uint32_t reg) {
    uint32_t val;

    if (cfg_read32(bus, dev, reg, &val) != CPL_STAT_SC) {
        halt(RESULT_CFG_FAILED);
    }
    return val;
}

//--------------------------------------------------------------------------
// BAR sizing and assignment. For each of the six BARs:
//   - write all ones, read back; 0 means the BAR is not implemented
//   - bit 0 set: I/O BAR. This RC has no I/O space -- left unassigned
//   - bits [2:1] = 10: 64-bit BAR, the next BAR holds the upper half
//   - size = ~(readback & ~0xF) + 1, the base is aligned to it
// BARs are packed into [win_base, win_base + win_size) in order. One that does
// not fit is not squeezed in but parked out of the way: a 64-bit BAR above
// 4 GB (out of reach of the 3-DW requests this RC sends), a 32-bit one at 0,
// below every window. Only BAR0 is used by the self-test.
//
// The Header Type says how many BARs there are: six for an endpoint (Type 0),
// two for a bridge (Type 1; from 0x18 on its bus numbers and windows),
// none for any other type.
//
// Memory decoding is switched off while the BARs move.
// Returns the address given to BAR0, or 0 when BAR0 could not be placed.
//--------------------------------------------------------------------------
uint32_t assign_bars(uint32_t bus, uint32_t dev,
                            uint32_t win_base, uint32_t win_size) {
    uint32_t next     = 0; // first free offset inside the window
    uint32_t bar0     = 0;
    uint32_t hdr      = (cfg_read32_checked(bus, dev, CFG_HEADER) >> 16) & 0x7F;
    int      nbars    = (hdr == 0) ? NUM_BARS : (hdr == 1) ? 2 : 0;

    cfg_write32(bus, dev, CFG_COMMAND, 0);

    for (int i = 0; i < nbars; i++) {
        uint32_t reg = CFG_BAR0 + 4 * i;

        cfg_write32(bus, dev, reg, 0xFFFFFFFF);
        uint32_t lo = cfg_read32_checked(bus, dev, reg);

        if (lo == 0) {
            continue;                             // not implemented
        }
        if (lo & 0x1) {
            cfg_write32(bus, dev, reg, 0);        // I/O -- left unassigned
            continue;
        }

        int      is64 = (((lo >> 1) & 0x3) == 0x2) && (i < nbars - 1);
        uint32_t hi   = 0xFFFFFFFF;               // upper half of the size mask

        if (is64) {
            cfg_write32(bus, dev, reg + 4, 0xFFFFFFFF);
            hi = cfg_read32_checked(bus, dev, reg + 4);
        }

        uint32_t size = ~(lo & 0xFFFFFFF0) + 1;   // 0 when 4 GB or more
        int      fits = 0;
        uint32_t base = 0;

        if (hi == 0xFFFFFFFF && size != 0 && size <= win_size) {
            base = (next + size - 1) & ~(size - 1);
            fits = (base <= win_size - size);
        }

        if (fits) {
            cfg_write32(bus, dev, reg, win_base + base);
            if (is64) {
                cfg_write32(bus, dev, reg + 4, 0);
            }
            next = base + size;
            if (i == 0) {
                bar0 = win_base + base;
            }
        } else if (is64) {
            // Base = size (aligned to itself), but never below 4 GB
            uint32_t park_hi = (hi == 0xFFFFFFFF) ? 1 : ~hi + 1;
            cfg_write32(bus, dev, reg, 0);
            cfg_write32(bus, dev, reg + 4, park_hi);
        } else {
            cfg_write32(bus, dev, reg, 0);
        }

        if (is64) {
            i++;                                  // upper half done
        }
    }
    return bar0;
}

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
