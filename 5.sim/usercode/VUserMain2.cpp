// SPDX-FileCopyrightText: 2025-2026 Chili.CHIPS
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// Copyright (c) 2025-2026 Chili.CHIPS
//--------------------------------------------------------------------------
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
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
//              https://opensource.org/license/bsd-3-clause
//--------------------------------------------------------------------------
// Description:
//   The Artix-7 RC model (make RC=model, tb_rc.sv): pcieVHost on node 2 as
//   the root complex, set up the way the Artix-7 PCIE_2_1 root port behaves
//   on the link (5.sim/README.md, "Profile: Artix-7 RC against pcieVHost").
//   It trains the link, brings up flow control, then runs our RC firmware
//   (3.sw/RC-direct): the same requests, paced as the firmware sends them
//   through the hard macro. It checks every Completion: tag, Requester ID,
//   status, byte count, and the values read back. With A7RC=1 one request
//   is outstanding at a time, as with the firmware; the tester also sends
//   them back to back.
//==========================================================================

#include <stdio.h>
#include <random>
#include <stdlib.h>

#include "VProcClass.h"
#include "pcieModelClass.h"

extern "C"
{
#include "ltssm.h"
}

#include "a7rc_check.h"
#define EP_DMA_CONSTANTS_ONLY
#include "ep_dma.h"


// ---- Artix-7 RC profile -------------------------------------------------
// Polling.Active TS1: 1025 with real timers; 17 with SIM_FAST_TRAIN, which our
// hard macro runs use
#define A7RC_POLL_ACTIVE_TS1  17
#define A7RC_N_FTS            255
// Disable Scrambling: our hard macro runs scramble only on hardware
// (SIM_PIPE_CODING); on the serial line the model scrambles, as hardware does
#ifdef SIM_SERIAL
#define A7RC_TS_CTL           0x00
#else
#define A7RC_TS_CTL           0x08
#endif
#define A7RC_SKP_INTERVAL     1474  // symbols
#define A7RC_ACK_TICKS        92    // Ack 98 symbol times after the TLP's END: with the link, the hard macro's 222 ns at Gen2 (make HM_PROBE=1)
#define A7RC_ACK_TICKS_X4     64    // x4: under Table 3-6's 73 symbol times (Gen1; Gen2 124) with the Ack's own; the hard macro's not measured
static int a7rc_ack_ticks = A7RC_ACK_TICKS;   // by the link's width (VUserMain2())
// Rates the RC advertises: 1 or 2 (make SIM_RC_GEN)
#ifndef RC_GEN
#define RC_GEN 2
#endif

// The tester may reverse its lanes (make SIM_LANE_REV=rc or both)
#ifndef LANE_REV_RC
#define LANE_REV_RC 0
#endif

// The tester: TS counts and idle times up to so many more than the least (SetLtssmVary())
#define A7RC_VARY             8

#define A7RC_CFG_IDLE_TICKS   17    // L0 (InitLinkGen() returns) to the Configuration.Idle SKP
#define A7RC_INITFC_TICKS     88    // then to the first InitFC: 657 ns after the last training TS
#define A7RC_P_HDR_CR         32
#define A7RC_P_DATA_CR        437
#define A7RC_NP_HDR_CR        12
#define A7RC_NP_DATA_CR       24    // Cpl: infinite, the model's default

// Mode: 0 (default), the tester: the lab RC's sequence, then stimulus it
// never sends but the spec allows (Stress()); 1 (make A7RC=1), literal:
// only what the lab RC sends
#ifndef A7RC_LITERAL
#define A7RC_LITERAL          0
#endif

// ---- the RC firmware (3.sw/RC-direct/main.c, 3.sw/common/pcie.c) ---------
#define FW_REQUESTER_ID       0x10ee
#define FW_EP                 (1 << 24)     // Configuration address: bus 1, device 0, function 0
#define FW_WINDOW_BASE        0x80000000    // the window the BARs are placed in
#define FW_WINDOW_SIZE        0x80000000
#define FW_TEST_DATA          0x00000006    // the self-test: written to BAR0, read back
#define FW_NUM_BARS           6
#define FW_CRS_RETRIES        100

#define CFG_ID                0x00
#define CFG_COMMAND           0x04
#define CFG_BAR0              0x10
#define CMD_MEM_BUS_MASTER    0x00000006

// Results, as the firmware writes them to PCIE_RESULT
#define RESULT_PASS           0x0000face
#define RESULT_FAIL           0x0000dead
#define RESULT_NO_DEVICE      0xbad00000
#define RESULT_CFG_FAILED     0xbad00002
#define RESULT_BAR0_FAILED    0xbad00003

#define CPL_TIMEOUT           8             // not a PCIE status: no Completion

// Firmware time, measured with the hard macro and eduBOS5: ns from the END of
// the endpoint's last TLP to the STP of the next request, at Gen2. At Gen1 the
// hard macro adds FW_GEN1_NS to each
// The first request, from L0 (the last training TS): the SOC leaves reset at
// DL_Active (core_rst_n follows trn_link_up), boots, then waits STARTUP_US.
// The firmware of our runs is built with SIM=1: STARTUP_US 75. On hardware it
// is 100000 (100 ms, 6.6.1), timed with rdcycle
#define FW_FIRST_NS           80958
#define FW_FIRST_GEN1_NS      74      // more at Gen1
#define FW_GEN1_NS            996
#define FW_CMD_OFF_NS         12172   // ID read, then Command = 0
#define FW_BAR_FIRST_NS       10084   // Command = 0, then BAR0 all ones
#define FW_SIZE_RD_NS         8996    // a BAR all ones, then its read back
#define FW_HI_RD_NS           9060    // the same, upper half of a 64-bit BAR
#define FW_PLACE_NS           14452   // read back, then the BAR's base
#define FW_HI_ONES_NS         12892   // read back, then the upper half all ones
#define FW_PARK_NS            12716   // upper half read back, then the lower half parked at 0
#define FW_PARK_HI_NS         9812    // then the upper half, parked above 4 GB
#define FW_NEXT_PLACED_NS     10724   // a BAR placed, then the next one all ones
#define FW_NEXT_BAR0_NS       256     // more, after BAR0
#define FW_NEXT_PARKED_NS     10484   // a 64-bit BAR parked, then the next one all ones
#define FW_NEXT_EMPTY_NS      12556   // a BAR not implemented, then the next one all ones
#define FW_CMD_ON_NS          12732   // the last BAR, then Command = Memory + Bus Master
#define FW_MWR_NS             6292    // then the self-test write
#define FW_MRD_NS             3984    // its read, from the write's STP: no Completion between
// Not measured: our endpoint takes none of these paths. They take the time of
// the nearest one above: an I/O BAR, a 64-bit BAR placed, a BAR that does not
// fit
#define FW_CRS_NS             373876  // CRS, then the request again (make SIM_FAULT=crs): CRS_WAIT_US, 374 in the SIM=1 build

// The model's own latency, taken off the firmware times: from a Completion's
// END on the wire to VUserInput_2(), plus from a request call to its STP;
// and from the last training TS on the wire to InitLinkGen()'s return, plus
// that call-to-STP time
#define FW_MODEL_RX_TX_NS     ((GetLinkSpeed(rc_node) == 1) ? 24 : 8)
#define FW_MODEL_L0_NS        194

// ---- the endpoint: what the checks expect ---------------------------------
// Another project's endpoint brings its own, -DA7RC_EP_EXPECT='"file.h"', with
// EP_VENDOR_ID, EP_DEVICE_ID and ep_bar_size[FW_NUM_BARS]. The default is ours,
// usercode/VUserMain1.cpp
#ifdef A7RC_EP_EXPECT
#include A7RC_EP_EXPECT
#else
#define EP_VENDOR_ID          0x14fc
#define EP_DEVICE_ID          0x0002

// BAR sizing reads: BAR0 4 KB, BAR1 1 KB, BAR2/3 64-bit 8 GB, prefetchable;
// BAR4, BAR5 not implemented
static const uint32_t ep_bar_size[FW_NUM_BARS] = {0xfffff008, 0xfffffc08, 0x0000000c, 0xfffffffe, 0x00000000, 0x00000000};
#endif

// The tester's memory requests stay in the first EP_SCRATCH_BYTES of each
// BAR. Ours is memory throughout; another project's endpoint sets its own,
// else only what Stress() writes
#ifndef EP_SCRATCH_BYTES
#ifdef A7RC_EP_EXPECT
#define EP_SCRATCH_BYTES      32
#else
#define EP_SCRATCH_BYTES      4096
#endif
#endif

// The firmware's Completion wait (CPL_TIMEOUT_US 2180 in the SIM=1 build):
// from the request's STP to its 0xBAD00002 (make SIM_FAULT=nocpl)
#define FW_CPL_TIMEOUT_NS     2178670

static int          rc_node   = 2;     // set by VUserMain2()
static int          tx_tag    = 0;     // the firmware's request count: tag = its low 5 bits
static bool         gen1_seen = false; // the link ran at Gen1 while the firmware ran

// The last Completion: count, tag, Requester ID, status, byte count and data,
// and the link time it came in
static int      cpl_count  = 0;
static int      cpl_tag    = 0;
static int      cpl_rid    = 0;
static int      cpl_status = 0;
static int      cpl_bytes  = 0;
static uint8_t  cpl_data[4096];
static uint64_t cpl_ns     = 0;
static uint64_t l0_ns      = 0;     // the link time training ended

// The read data of each Completion by its tag, and the tags seen: for
// requests sent back to back (Stress())
static uint32_t cpl_tag_data[32];
static uint32_t cpl_tag_seen = 0;

static int      errors     = 0;

// The BARs the firmware placed in the window: base and size (0: not placed)
static uint32_t bar_base[FW_NUM_BARS];
static uint32_t bar_size[FW_NUM_BARS];

// Random stimulus: one seed for all of it, make SEED=N (A7RC_SEED)
static std::mt19937 rng;

static uint32_t Rand(const uint32_t n)
{
    return rng() % n;
}

// Stimulus coverage: how often each kind ran, printed at the end
enum
{
    S_FW, S_CFG_RD, S_CFG_WR, S_B2B_WR, S_B2B_RD, S_FILL, S_MIX_WR, S_MIX_RD, S_MULTI_DW, S_PARTIAL_BE, S_AT_4KB,
    S_GAP_NONE, S_GAP_RANDOM, S_BURST, S_OUT_MAX, S_UR_NP, S_UR_P, S_VDM1, S_ERR_CLR, S_ECRC, S_POISON, S_TC_ATTR, S_NAK, S_ACK_HOLD, S_BAD_LCRC, S_DUP, S_BAD_DLLP, S_EP_REQ, S_FC_HOLD,
    S_SPEED, S_RETRAIN, S_RETRAIN_SPEED,
    S_NUM
};

static const char *stim_name[S_NUM] =
{
    "lab RC firmware requests", "config sweep reads", "config sweep writes", "back-to-back writes",
    "back-to-back reads", "mix: fill writes", "mix: writes", "mix: reads", "mix: more than 1 DW", "mix: partial Byte Enables",
    "mix: ending at a 4 KB boundary", "mix: no gap before", "mix: a random gap before", "mix: bursts (4-12, mostly reads)",
    "mix: most requests outstanding", "unsupported: non-posted (UR)",
    "unsupported: posted (dropped)", "vendor message type 1 (dropped)", "error status cleared (RW1C)", "ECRC (TD 1) requests",
    "poisoned writes", "reads with TC 1-7, RO/NS", "Naks of good TLPs", "Acks held past the replay timer",
    "TLPs with a bad LCRC", "duplicate TLPs", "DLLPs with a bad CRC", "the endpoint's requests (DMA)",
    "UpdateFCs held (credit starvation)", "speed changes, reads outstanding",
    "Recovery at a random point", "... with a speed change",
};

static unsigned stim[S_NUM];

// The firmware's halt(): thrown with the result marker
struct Halt
{
    uint32_t marker;
};

// openPCIE: the reset level, read in a delta cycle (no clock). Upstream waited
// for an interrupt on the release edge, registered after the setup: an
// earlier release was lost, and the model waited for ever (issue 57)
static bool InReset(VProc *vp)
{
    unsigned rst;

    vp->read(RESET_STATE, &rst, 1);
    return rst & 1;
}

//-------------------------------------------------------------
// VUserInput_2()
//
// Keeps the fields of each Completion for WaitForCpl()
//-------------------------------------------------------------

static void VUserInput_2(pPkt_t pkt, int status, void* usrptr)
{
    if (pkt->seq != DLLP_SEQ_ID)
    {
        int type = GET_TLP_TYPE(pkt->data) & TLP_TYPE_MASK;

        if (type == TL_CPL || type == TL_CPLD || type == TL_CPLLK || type == TL_CPLDLK)   // CplLk: an MRdLk's UR
        {
            cpl_tag    = GET_CPL_TAG(pkt->data);
            cpl_rid    = (pkt->data[CPL_RID_OFFSET] << 8) | pkt->data[CPL_RID_OFFSET + 1];
            cpl_status = GET_CPL_STATUS(pkt->data);
            cpl_bytes  = pkt->ByteCount;
            cpl_ns     = GetLinkTimeNs(rc_node);

            for (int idx = 0; idx < pkt->ByteCount && idx < (int)sizeof(cpl_data); idx++)
            {
                cpl_data[idx] = GET_TLP_PAYLOAD_PTR(pkt->data)[idx];
            }
            cpl_tag_data[cpl_tag & 0x1f] = (cpl_bytes >= 4) ? (((uint32_t)cpl_data[0] << 24) | (cpl_data[1] << 16) |
                                                         (cpl_data[2] << 8) | cpl_data[3]) : 0;
            cpl_tag_seen |= 1u << (cpl_tag & 0x1f);
            cpl_count++;
        }
        else
        {
            VPrint("**** ERROR: RC model: unexpected TLP of type 0x%02x from the endpoint\n", type);
            errors++;
        }
    }

    DISCARD_PACKET(pkt);
}

//-------------------------------------------------------------
// FwWait()
//
// The firmware's time: waits until 'wait_ns' after link time 'from',
// less the model's own latency 'model_ns'
//-------------------------------------------------------------

static void FwWait(const uint64_t from, const int wait_ns, const int model_ns)
{
    while (GetLinkTimeNs(rc_node) + model_ns < from + wait_ns)
    {
        LinkIdle(1, rc_node);    // follows the endpoint into Recovery
        gen1_seen |= (GetLinkSpeed(rc_node) == 1);
    }
}

//-------------------------------------------------------------
// WaitForCpl()
//
// Waits for the Completion of the request with 'tag', and checks it:
// one Completion, its tag and Requester ID, and on success 'bytes' of data
// (0 for a write). Returns its status, or CPL_TIMEOUT.
//-------------------------------------------------------------

static int WaitForCpl(const int tag, const int bytes, const char *what)
{
    int      start   = cpl_count;
    uint64_t sent_ns = GetLinkTimeNs(rc_node);

    while (cpl_count == start)
    {
        if (GetLinkTimeNs(rc_node) >= sent_ns + FW_CPL_TIMEOUT_NS)
        {
            VPrint("**** ERROR: RC model: no Completion for %s (tag %d)\n", what, tag);
            errors++;
            return CPL_TIMEOUT;
        }
        LinkIdle(1, rc_node);
        gen1_seen |= (GetLinkSpeed(rc_node) == 1);
    }

    if (cpl_count != start + 1 || cpl_tag != tag || cpl_rid != FW_REQUESTER_ID ||
        (cpl_status == CPL_SUCCESS && cpl_bytes != bytes))
    {
        VPrint("**** ERROR: RC model: %s: %d Completion(s), tag %d (expected %d), Requester ID 0x%04x, status %d, %d bytes (expected %d)\n",
               what, cpl_count - start, cpl_tag, tag, cpl_rid, cpl_status, cpl_bytes, bytes);
        errors++;
    }

    return cpl_status;
}

//-------------------------------------------------------------
// Request()
//
// pcie_request(): one non-posted request, 'wait_ns' of firmware time
// after the endpoint's last TLP, and its Completion. Re-issued, with a
// new tag, while the endpoint answers CRS. Configuration data travels
// little-endian (the firmware's bswap32()), memory data big-endian.
// Returns the Completion Status; the data read in *rdata.
//-------------------------------------------------------------

static int Request(pcieModelClass *pcie, const int type, const uint32_t addr, const uint32_t wdata,
                   uint32_t *rdata, int wait_ns, const char *what)
{
    const bool cfg    = (type == TL_CFGRD0 || type == TL_CFGWR0);
    const int  bytes  = (type == TL_CFGWR0) ? 0 : 4;
    int        status = CPL_TIMEOUT;

    for (int retry = 0; retry <= FW_CRS_RETRIES; retry++)
    {
        int tag = tx_tag++ & 0x1f;

        FwWait(cpl_ns, wait_ns + ((GetLinkSpeed(rc_node) == 1) ? FW_GEN1_NS : 0), FW_MODEL_RX_TX_NS);

        switch (type)
        {
        case TL_CFGRD0:
            pcie->cfgRead(addr, 4, tag, FW_REQUESTER_ID, SEND);
            break;
        case TL_CFGWR0:
        {
            PktData_t buf[4] = {(PktData_t)(wdata & 0xff), (PktData_t)((wdata >> 8) & 0xff),
                                (PktData_t)((wdata >> 16) & 0xff), (PktData_t)(wdata >> 24)};
            pcie->cfgWrite(addr, buf, 4, tag, FW_REQUESTER_ID, SEND);
            break;
        }
        default:
            pcie->memRead(addr, 4, tag, FW_REQUESTER_ID, SEND);
            break;
        }

        status = WaitForCpl(tag, bytes, what);

        if (status != CPL_CRS)
        {
            break;
        }
        wait_ns = FW_CRS_NS;
    }

    if (status == CPL_SUCCESS && rdata)
    {
        *rdata = cfg ? (cpl_data[0] | (cpl_data[1] << 8) | (cpl_data[2] << 16) | ((uint32_t)cpl_data[3] << 24))
                     : (((uint32_t)cpl_data[0] << 24) | (cpl_data[1] << 16) | (cpl_data[2] << 8) | cpl_data[3]);
    }

    return status;
}

//-------------------------------------------------------------
// CfgWr32() / CfgRd32()
//
// The firmware's checked Configuration write and read: any status
// other than success halts it with RESULT_CFG_FAILED
//-------------------------------------------------------------

static void CfgWr32(pcieModelClass *pcie, const int reg, const uint32_t val, const int wait_ns, const char *what)
{
    if (Request(pcie, TL_CFGWR0, FW_EP | reg, val, NULL, wait_ns, what) != CPL_SUCCESS)
    {
        throw Halt{RESULT_CFG_FAILED};
    }
}

static uint32_t CfgRd32(pcieModelClass *pcie, const int reg, const int wait_ns, const char *what)
{
    uint32_t val = 0;

    if (Request(pcie, TL_CFGRD0, FW_EP | reg, 0, &val, wait_ns, what) != CPL_SUCCESS)
    {
        throw Halt{RESULT_CFG_FAILED};
    }

    return val;
}

static void Check(const char *what, const uint32_t got, const uint32_t expected)
{
    if (got != expected)
    {
        VPrint("**** ERROR: RC model: %s = 0x%08x, expected 0x%08x\n", what, got, expected);
        errors++;
    }
    else
    {
        VPrint("===> RC model: %s = 0x%08x\n", what, got);
    }
}

//-------------------------------------------------------------
// AssignBars()
//
// assign_bars(): sizes the six BARs and packs them into the window,
// in order; parks one that does not fit. Memory decoding is off
// meanwhile. Returns BAR0's address, 0 when BAR0 could not be placed.
// Also checks each sizing read against this endpoint (ep_bar_size).
//-------------------------------------------------------------

static uint32_t AssignBars(pcieModelClass *pcie)
{
    uint32_t next = 0;                         // first free offset in the window
    uint32_t bar0 = 0;
    int      wait = FW_BAR_FIRST_NS;           // firmware time to the next BAR, by the path through this one
    char     what[32];

    CfgWr32(pcie, CFG_COMMAND, 0, FW_CMD_OFF_NS, "Command off");

    for (int i = 0; i < FW_NUM_BARS; i++)
    {
        int reg = CFG_BAR0 + 4 * i;

        snprintf(what, sizeof(what), "BAR%d size", i);
        CfgWr32(pcie, reg, 0xffffffff, wait, what);
        uint32_t lo = CfgRd32(pcie, reg, FW_SIZE_RD_NS, what);
        Check(what, lo, ep_bar_size[i]);

        if (lo == 0)                           // not implemented
        {
            wait = FW_NEXT_EMPTY_NS;
            continue;
        }
        if (lo & 1)                            // I/O: left unassigned
        {
            CfgWr32(pcie, reg, 0, FW_PLACE_NS, what);
            wait = FW_NEXT_PLACED_NS;
            continue;
        }

        bool     is64 = (((lo >> 1) & 3) == 2) && (i < FW_NUM_BARS - 1);
        uint32_t hi   = 0xffffffff;            // upper half of the size mask

        if (is64)
        {
            snprintf(what, sizeof(what), "BAR%d size", i + 1);
            CfgWr32(pcie, reg + 4, 0xffffffff, FW_HI_ONES_NS, what);
            hi = CfgRd32(pcie, reg + 4, FW_HI_RD_NS, what);
            Check(what, hi, ep_bar_size[i + 1]);
        }

        uint32_t size = ~(lo & 0xfffffff0) + 1; // 0 when 4 GB or more
        uint32_t base = 0;
        bool     fits = false;

        if (hi == 0xffffffff && size != 0 && size <= FW_WINDOW_SIZE)
        {
            base = (next + size - 1) & ~(size - 1);
            fits = (base <= FW_WINDOW_SIZE - size);
        }

        snprintf(what, sizeof(what), "BAR%d address", i);
        if (fits)
        {
            CfgWr32(pcie, reg, FW_WINDOW_BASE + base, is64 ? FW_PARK_NS : FW_PLACE_NS, what);
            if (is64)
            {
                CfgWr32(pcie, reg + 4, 0, FW_PARK_HI_NS, what);
            }
            next = base + size;
            bar_base[i] = FW_WINDOW_BASE + base;
            bar_size[i] = size;
            if (i == 0)
            {
                bar0 = FW_WINDOW_BASE + base;
            }
            wait = is64 ? FW_NEXT_PARKED_NS : FW_NEXT_PLACED_NS + ((i == 0) ? FW_NEXT_BAR0_NS : 0);
        }
        else if (is64)
        {
            // Base = size (aligned to itself), but never below 4 GB
            CfgWr32(pcie, reg, 0, FW_PARK_NS, what);
            CfgWr32(pcie, reg + 4, (hi == 0xffffffff) ? 1 : ~hi + 1, FW_PARK_HI_NS, what);
            wait = FW_NEXT_PARKED_NS;
        }
        else
        {
            CfgWr32(pcie, reg, 0, FW_PLACE_NS, what);
            wait = FW_NEXT_PLACED_NS;
        }

        if (is64)
        {
            i++;                               // upper half done
        }
    }

    return bar0;
}

//-------------------------------------------------------------
// Firmware()
//
// main() of 3.sw/RC-direct: finds the endpoint, places its BARs,
// enables it, then writes FW_TEST_DATA to BAR0 and reads it back.
// Returns the result marker.
//-------------------------------------------------------------

static uint32_t Firmware(pcieModelClass *pcie)
{
    uint32_t id;

    FwWait(l0_ns, FW_FIRST_NS + ((GetLinkSpeed(rc_node) == 1) ? FW_FIRST_GEN1_NS : 0), FW_MODEL_L0_NS);
    if (Request(pcie, TL_CFGRD0, FW_EP | CFG_ID, 0, &id, 0, "Vendor/Device ID") != CPL_SUCCESS || id == 0xffffffff)
    {
        return RESULT_NO_DEVICE;
    }
    Check("Vendor/Device ID", id, (EP_DEVICE_ID << 16) | EP_VENDOR_ID);

    uint32_t test_addr = AssignBars(pcie);
    if (test_addr == 0)
    {
        return RESULT_BAR0_FAILED;
    }
    Check("BAR0 address", test_addr, FW_WINDOW_BASE);

    CfgWr32(pcie, CFG_COMMAND, CMD_MEM_BUS_MASTER, FW_CMD_ON_NS, "Command");

    // The self-test: a posted write, then its read
    PktData_t buf[4] = {0, 0, 0, FW_TEST_DATA};    // big-endian
    uint32_t  readback;

    FwWait(cpl_ns, FW_MWR_NS + ((GetLinkSpeed(rc_node) == 1) ? FW_GEN1_NS : 0), FW_MODEL_RX_TX_NS);
    uint64_t mwr_ns = GetLinkTimeNs(rc_node);
    pcie->memWrite(test_addr, buf, 4, tx_tag++ & 0x1f, FW_REQUESTER_ID, SEND);

    FwWait(mwr_ns, FW_MRD_NS, 0);
    if (Request(pcie, TL_MRD32, test_addr, 0, &readback, 0, "BAR0 self-test") == CPL_SUCCESS &&
        readback == FW_TEST_DATA)
    {
        return RESULT_PASS;
    }

    return RESULT_FAIL;
}

#if !A7RC_LITERAL
//-------------------------------------------------------------
// CfgSweep()
//
// The tester (not A7RC=1): after the firmware, every DW of 00h-FFh, the
// extended capability chain from 100h, writes to read-only registers and to
// the RW1C status bits, each read back, and a read of function 1 if the
// endpoint is single-function. Any status is taken: the checker
// (a7rc_check.cpp, its CFG. rules) judges the answers.
//-------------------------------------------------------------

#define CFG_SWEEP_DWS         64              // 00h-FFh: the header and the capabilities

static uint32_t CfgSweepRd(pcieModelClass *pcie, const int reg)
{
    uint32_t val = 0;
    stim[S_CFG_RD]++;
    Request(pcie, TL_CFGRD0, FW_EP | reg, 0, &val, 0, "config sweep read");
    return val;
}

static void CfgSweepWr(pcieModelClass *pcie, const int reg, const uint32_t val)
{
    stim[S_CFG_WR]++;
    Request(pcie, TL_CFGWR0, FW_EP | reg, val, NULL, 0, "config sweep write");
    CfgSweepRd(pcie, reg);
}

static void CfgSweep(pcieModelClass *pcie)
{
    uint32_t hdr[CFG_SWEEP_DWS];

    for (int i = 0; i < CFG_SWEEP_DWS; i++)
    {
        hdr[i] = CfgSweepRd(pcie, 4 * i);
    }

    // The extended capability chain, at most 16 entries
    uint32_t off = 0x100;
    for (int n = 0; n < 16 && off; n++)
    {
        uint32_t next = CfgSweepRd(pcie, off) >> 20;
        off           = (next >= 0x100 && !(next & 3)) ? next : 0;
    }

    // Read-only: the IDs, Class Code, Subsystem IDs, Capabilities Pointer;
    // in 00Ch only Latency Timer and Header Type (BIST could start a test)
    static const int ro_regs[] = {0x00, 0x08, 0x2c, 0x34};
    for (int reg : ro_regs)
    {
        CfgSweepWr(pcie, reg, 0xffffffff);
    }
    CfgSweepWr(pcie, 0x0c, (hdr[3] & 0xff0000ff) | 0x00ffff00);

    // RW1C: Status, and the PCIe Capability's Device Status; the control
    // halves written back as read
    CfgSweepWr(pcie, CFG_COMMAND, 0xffff0000 | (hdr[1] & 0xffff));
    uint32_t ptr = hdr[0x0d] & 0xfc;
    for (int n = 0; n < 48 && ptr >= 0x40; n++)
    {
        if ((hdr[ptr / 4] & 0xff) == 0x10)
        {
            CfgSweepWr(pcie, ptr + 8, 0xffff0000 | (hdr[ptr / 4 + 2] & 0xffff));
            // read-only: PCIe Capabilities, Device and Link Capabilities (CFG.RO)
            CfgSweepWr(pcie, ptr, 0xffffffff);
            CfgSweepWr(pcie, ptr + 4, 0xffffffff);
            CfgSweepWr(pcie, ptr + 12, 0xffffffff);
            break;
        }
        ptr = (hdr[ptr / 4] >> 8) & 0xfc;
    }

    // Function 1 of a single-function device
    if (!(hdr[3] & 0x00800000))
    {
        ChkExpectUr(tx_tag & 0x1f);              // a function the device lacks: UR (CFG.FN, TL.UR)
        Request(pcie, TL_CFGRD0, FW_EP | (1 << 16), 0, NULL, 0, "function 1");
    }
}

//-------------------------------------------------------------
// Stress()
//
// The tester (not A7RC=1): after the firmware,
// B2B_COUNT memory writes to BAR0, then B2B_COUNT reads of them, each TLP
// sent right after the one before, then the data checked. Legal: the
// framing rules set no gap between packets, only flow control limits the
// sender (the model waits for credits). The lab RC's firmware leaves
// microseconds between its requests, so it never sends these.
//-------------------------------------------------------------

#define B2B_COUNT             8               // as many as our EP's P and NP header credits
#define B2B_DATA(_i)          (0xb2b00000 | (_i))

static void Stress(pcieModelClass *pcie, const uint32_t bar0)
{
    int  tags[B2B_COUNT];
    char what[32];

    for (int i = 0; i < B2B_COUNT; i++)
    {
        uint32_t  d      = B2B_DATA(i);
        PktData_t buf[4] = {(PktData_t)(d >> 24), (PktData_t)((d >> 16) & 0xff),
                            (PktData_t)((d >> 8) & 0xff), (PktData_t)(d & 0xff)};    // big-endian
        pcie->memWrite(bar0 + 4 * i, buf, 4, tx_tag++ & 0x1f, FW_REQUESTER_ID, SEND);
        stim[S_B2B_WR]++;
    }

    cpl_tag_seen = 0;
    uint32_t want = 0;
    for (int i = 0; i < B2B_COUNT; i++)
    {
        tags[i] = tx_tag++ & 0x1f;
        want   |= 1u << tags[i];
        pcie->memRead(bar0 + 4 * i, 4, tags[i], FW_REQUESTER_ID, SEND);
        stim[S_B2B_RD]++;
    }

    uint64_t sent_ns = GetLinkTimeNs(rc_node);
    while ((cpl_tag_seen & want) != want && GetLinkTimeNs(rc_node) < sent_ns + FW_CPL_TIMEOUT_NS)
    {
        LinkIdle(1, rc_node);
    }

    for (int i = 0; i < B2B_COUNT; i++)
    {
        snprintf(what, sizeof(what), "back-to-back read %d", i);
        if (!(cpl_tag_seen & (1u << tags[i])))
        {
            VPrint("**** ERROR: RC model: no Completion for %s (tag %d)\n", what, tags[i]);
            errors++;
        }
        else
        {
            Check(what, cpl_tag_data[tags[i]], B2B_DATA(i));
        }
    }
}

//-------------------------------------------------------------
// Unsupported()
//
// The tester (not A7RC=1): requests the endpoint must not support (2.3.1),
// each legal on the link: IO read and write (no I/O BAR), MRdLk (a PCI
// Express Endpoint supports no locks, 6.5), a memory read and write outside
// the BARs, a Type 1 Configuration Read, Vendor-Defined Messages Type 0 (UR)
// and Type 1 (dropped silently, 2.2.8.6). Then Device Status: UR Detected must
// be set (7.8.5); a write of 0 keeps it, a write of 1 clears it (RW1C, 7.4).
// Then an ordinary read: the endpoint keeps working. The checker judges
// (TL.UR, CFG.ERR-STATUS, CFG.RW1C, TL.CPL-MATCH: no Completion for posted).
//-------------------------------------------------------------

#ifndef BIT7MASK
#define BIT7MASK              0x80      // TLP_HAS_DIGEST(); pcie_utils.h is C only
#endif
extern "C" void CalcLcrc(PktData_t *pkt);
extern "C" void CalcEcrc(PktData_t *pkt);

#define UR_IO_ADDR            0x00001000

static int UrTag(void)
{
    int tag = tx_tag++ & 0x1f;
    ChkExpectUr(tag);
    stim[S_UR_NP]++;
    return tag;
}

static void UrWait(const int tag, const char *what)
{
    int status = WaitForCpl(tag, 0, what);
    VPrint("===> RC model: %s: Completion status %d (%s)\n", what, status, status == CPL_UNSUPPORTED ? "UR" : "not UR");
}

// Device Status (PCIe Capability + 8): read it, the checker judges (CFG.ERR-
// STATUS); then clear its RW1C bits: a 0 written first must keep them
static void DevStatusCheck(pcieModelClass *pcie, const int reg)
{
    uint32_t ds = CfgSweepRd(pcie, reg);
    if (ds >> 16)
    {
        CfgSweepWr(pcie, reg, ds & 0x0000ffff);                 // RW1C: a 0 keeps them
        CfgSweepWr(pcie, reg, (ds & 0x0000ffff) | 0x000f0000);  // a 1 clears them
        stim[S_ERR_CLR]++;
    }
}

static void Unsupported(pcieModelClass *pcie)
{
    PktData_t d4[4] = {0x12, 0x34, 0x56, 0x78};
    int       tag;

    // An address past every BAR placed, on a 1 MB boundary
    uint32_t top = FW_WINDOW_BASE;
    for (int i = 0; i < FW_NUM_BARS; i++)
    {
        if (bar_size[i] && bar_base[i] + bar_size[i] > top)
        {
            top = bar_base[i] + bar_size[i];
        }
    }
    const uint32_t ur_mem = (top + 0xfffff) & ~0xfffffu;

    // Device Status: found, then cleared, so each group below is judged alone
    int ds = 0;
    for (uint32_t ptr = CfgSweepRd(pcie, 0x34) & 0xfc, n = 0; n < 48 && ptr >= 0x40; n++)
    {
        uint32_t cap = CfgSweepRd(pcie, ptr);
        if ((cap & 0xff) == 0x10)
        {
            ds = ptr + 8;
            break;
        }
        ptr = (cap >> 8) & 0xfc;
    }
    if (ds)
    {
        DevStatusCheck(pcie, ds);
        CfgSweepRd(pcie, ds);
    }

    // Non-posted: each gets UR
    tag = UrTag(); pcie->ioRead(UR_IO_ADDR, 4, tag, FW_REQUESTER_ID, SEND);            UrWait(tag, "IO read");
    tag = UrTag(); pcie->ioWrite(UR_IO_ADDR, d4, 4, tag, FW_REQUESTER_ID, SEND);       UrWait(tag, "IO write");
    tag = UrTag(); pcie->memRead(FW_WINDOW_BASE, 4, tag, FW_REQUESTER_ID, SEND, false, true);
    UrWait(tag, "MRdLk");
    tag = UrTag(); pcie->memRead(ur_mem, 4, tag, FW_REQUESTER_ID, SEND);               UrWait(tag, "memory read outside the BARs");

    // Type 1: built as a Type 0 read, queued, its type changed, the CRCs again
    tag = UrTag();
    PktData_t *p = pcie->cfgRead(FW_EP | 0x00, 4, tag, FW_REQUESTER_ID, true);
    p[TLP_TYPE_BYTE_OFFSET] = (p[TLP_TYPE_BYTE_OFFSET] & 0xe0) | 0x05;
    if (TLP_HAS_DIGEST(p))
    {
        CalcEcrc(p);
    }
    CalcLcrc(p);
    pcie->sendPacket();
    UrWait(tag, "Type 1 Configuration Read");
    if (ds)
    {
        DevStatusCheck(pcie, ds);
    }

    // Posted, one at a time: no Completion; UR Detected but for a Type 1 VDM
    pcie->memWrite(ur_mem, d4, 4, 0, FW_REQUESTER_ID, SEND);
    ChkUrPosted();
    stim[S_UR_P]++;
    LinkIdle(200, rc_node);
    if (ds)
    {
        DevStatusCheck(pcie, ds);
    }
    pcie->message(MSG_VENDOR_0, NULL, 0, 0, FW_REQUESTER_ID, 0);
    ChkUrPosted();
    stim[S_UR_P]++;
    LinkIdle(200, rc_node);
    if (ds)
    {
        DevStatusCheck(pcie, ds);
    }
    pcie->message(MSG_VENDOR_1, NULL, 0, 0, FW_REQUESTER_ID, 0);
    stim[S_VDM1]++;
    LinkIdle(200, rc_node);
    if (ds)
    {
        DevStatusCheck(pcie, ds);                               // clear: the checker expects 0
    }

    // Still working
    uint32_t v;
    Request(pcie, TL_MRD32, FW_WINDOW_BASE, 0, &v, 0, "a read after the Unsupported Requests");
}

//-------------------------------------------------------------
// Odd()
//
// The tester (not A7RC=1): legal TLPs our firmware never sends. A write and a
// read with ECRC (TD 1, 2.7.1): the endpoint takes them, and answers without
// ECRC (no AER: generation off). A poisoned write (2.7.2): its data may or may
// not land; Status Detected Parity Error and, in Device Status, its severity
// are set (checked and cleared, RW1C, before the next). A poisoned
// Configuration Write gets UR, logged as its UR, and changes nothing. Reads
// with Traffic Class 1-7 (all map
// to VC0 without a VC Capability) and Relaxed Ordering / No Snoop: the
// Completion carries the request's TC and Attr (2.2.9). The checker judges
// (TL.ORDER, TL.ECRC, CFG.ERR-STATUS, TL.CPL-TC-ATTR).
//-------------------------------------------------------------

static void Odd(pcieModelClass *pcie)
{
    const uint32_t a = bar_base[0] + 0x10;      // inside every scratch span
    PktData_t      w[8], pw[4] = {0xde, 0xad, 0xbe, 0xef};
    int            tag;

    // Status, and Device Status (PCIe Capability + 8): read, the checker
    // judges (CFG.ERR-STATUS), then cleared (RW1C)
    int ds = 0;
    for (uint32_t ptr = CfgSweepRd(pcie, 0x34) & 0xfc, n = 0; n < 48 && ptr >= 0x40; n++)
    {
        uint32_t cap = CfgSweepRd(pcie, ptr);
        if ((cap & 0xff) == 0x10)
        {
            ds = ptr + 8;
            break;
        }
        ptr = (cap >> 8) & 0xfc;
    }
    auto errs = [&]()
    {
        uint32_t st = CfgSweepRd(pcie, CFG_COMMAND);
        if (st >> 16)
        {
            CfgSweepWr(pcie, CFG_COMMAND, (st & 0x0000ffff) | 0xf9000000);   // RW1C: clear, read back (CFG.RW1C)
            stim[S_ERR_CLR]++;
        }
        if (ds)
        {
            DevStatusCheck(pcie, ds);
        }
    };

    for (int i = 0; i < 8; i++)
    {
        w[i] = Rand(256);
    }

    // ECRC
    pcie->memWriteDigest(a, w, 8, 0, FW_REQUESTER_ID, true, SEND);
    tag = tx_tag++ & 0x1f;
    pcie->memReadDigest(a, 8, tag, FW_REQUESTER_ID, true, SEND);
    WaitForCpl(tag, 8, "a read with ECRC");
    stim[S_ECRC] += 2;

    // Poisoned: built as a write, queued, its EP bit set, the LCRC again
    PktData_t *p = pcie->memWrite(a, pw, 4, 0, FW_REQUESTER_ID, true);
    p[TLP_TYPE_BYTE_OFFSET + 2] |= 0x40;
    CalcLcrc(p);
    pcie->sendPacket();
    stim[S_POISON]++;
    tag = tx_tag++ & 0x1f;
    pcie->memRead(a, 4, tag, FW_REQUESTER_ID, SEND);
    WaitForCpl(tag, 4, "a read after a poisoned write");
    errs();                                     // the poisoned write alone

    // A poisoned Configuration Write: discarded, UR (2.7.2.2). Interrupt Line
    // (RW) must keep its value
    uint32_t il = CfgSweepRd(pcie, 0x3c);
    PktData_t cw[4] = {(PktData_t)(~il & 0xff), (PktData_t)((il >> 8) & 0xff), (PktData_t)((il >> 16) & 0xff),
                       (PktData_t)(il >> 24)};
    tag = UrTag();
    p   = pcie->cfgWrite(FW_EP | 0x3c, cw, 4, tag, FW_REQUESTER_ID, true);
    p[TLP_TYPE_BYTE_OFFSET + 2] |= 0x40;
    CalcLcrc(p);
    pcie->sendPacket();
    stim[S_POISON]++;
    UrWait(tag, "poisoned Configuration Write");
    if (CfgSweepRd(pcie, 0x3c) != il)
    {
        VPrint("**** ERROR: RC model: a poisoned Configuration Write changed Interrupt Line\n");
        errors++;
    }

    errs();                                     // the poisoned Configuration Write alone

    // Traffic Class and attributes, set in the queued request's header
    for (int tc = 1; tc < 8; tc++)
    {
        tag = tx_tag++ & 0x1f;
        p   = pcie->memRead(a, 4, tag, FW_REQUESTER_ID, true);
        p[TLP_TYPE_BYTE_OFFSET + 1] = (p[TLP_TYPE_BYTE_OFFSET + 1] & 0x8f) | (tc << 4);
        p[TLP_TYPE_BYTE_OFFSET + 2] = (p[TLP_TYPE_BYTE_OFFSET + 2] & 0xcf) | (Rand(4) << 4);
        CalcLcrc(p);
        pcie->sendPacket();
        WaitForCpl(tag, 4, "a read with TC and attributes");
        stim[S_TC_ATTR]++;
    }
}


//-------------------------------------------------------------
// Faults()
//
// The tester (not A7RC=1): Data Link faults from the RC, one at a time, each
// around reads of the scratch span, whose Completions must arrive once
// (3.5.2.1, 3.5.3.1). The RC Naks a good Completion: the endpoint sends it
// again, and those after it, in order. The RC holds its Acks past the
// endpoint's replay timer: the endpoint replays, on time. A TLP with a bad
// LCRC: the endpoint Naks it, the RC replays. A duplicate TLP: Acked,
// dropped. A DLLP with a bad CRC: ignored. The checker judges (DL.REPLAY,
// DL.REPLAY-ORDER, DL.BAD-TLP, DL.DUPLICATE, TL.CPL-MATCH, TL.CPL-ALL).
//-------------------------------------------------------------

// A line per fault, with the time: where to look in the waveform
static void FaultMark(const char *what)
{
    VPrint("===> Fault at %.3f us: %s\n", GetLinkTimeNs(rc_node) / 1000.0, what);
}

static void FaultReads(pcieModelClass *pcie, const int n, const char *what)
{
    int tags[4];
    for (int i = 0; i < n; i++)
    {
        tags[i] = tx_tag++ & 0x1f;
        pcie->memRead(bar_base[0] + 0x10, 4, tags[i], FW_REQUESTER_ID, SEND);
    }
    for (int i = 0; i < n; i++)
    {
        WaitForCpl(tags[i], 4, what);
    }
}

static void Faults(pcieModelClass *pcie)
{
    // Nak: the first of four Completions; the others follow it again
    FaultMark("the RC Naks the next Completion");
    InjectFault(FAULT_RX_NAK_NEXT, rc_node);
    FaultReads(pcie, 4, "a read around a Nak");
    stim[S_NAK]++;

    // Acks held past the replay timer (Gen2 864 symbols): replays
    FaultMark("the RC holds its Acks");
    pcie->configurePcie(CONFIG_ENABLE_ACK, 3 * REPLAY_TIMER_GEN2);
    FaultReads(pcie, 2, "a read with the Acks held");
    LinkIdle(3 * REPLAY_TIMER_GEN2, rc_node);   // the Completions are in: hold their Acks on
    pcie->configurePcie(CONFIG_ENABLE_ACK, a7rc_ack_ticks);
    LinkIdle(4 * REPLAY_TIMER_GEN2, rc_node);
    stim[S_ACK_HOLD]++;

    // A bad LCRC on the next request (the replay is good)
    FaultMark("the RC sends a TLP with a bad LCRC");
    ChkFault(FAULT_TX_BAD_LCRC);
    InjectFault(FAULT_TX_BAD_LCRC, rc_node);
    FaultReads(pcie, 1, "a read sent with a bad LCRC");
    stim[S_BAD_LCRC]++;

    // A request sent again after its Ack: a duplicate, to Ack and drop
    InjectFault(FAULT_TX_DUP, rc_node);
    FaultReads(pcie, 1, "a read later sent again");
    LinkIdle(2 * REPLAY_TIMER_GEN2, rc_node);
    FaultMark("the RC sends a TLP again after its Ack");
    ChkFault(FAULT_TX_DUP);
    SendDuplicate(rc_node);
    LinkIdle(2 * REPLAY_TIMER_GEN2, rc_node);
    stim[S_DUP]++;

    // A bad CRC on the next DLLP (an Ack or an UpdateFC)
    FaultMark("the RC sends a DLLP with a bad CRC");
    ChkFault(FAULT_TX_BAD_DLLP);
    InjectFault(FAULT_TX_BAD_DLLP, rc_node);
    FaultReads(pcie, 2, "a read around a bad DLLP");
    LinkIdle(4 * REPLAY_TIMER_GEN2, rc_node);
    stim[S_BAD_DLLP]++;

    // The endpoint's Correctable errors (Bad TLP, Bad DLLP, Replay Timer
    // Timeout): Correctable Error Detected in Device Status (CFG.ERR-STATUS)
    for (uint32_t ptr = CfgSweepRd(pcie, 0x34) & 0xfc, n = 0; n < 48 && ptr >= 0x40; n++)
    {
        uint32_t cap = CfgSweepRd(pcie, ptr);
        if ((cap & 0xff) == 0x10)
        {
            DevStatusCheck(pcie, ptr + 8);
            break;
        }
        ptr = (cap >> 8) & 0xfc;
    }
}

//-------------------------------------------------------------
// Dma()
//
// The tester (not A7RC=1): the endpoint's own requests. The RC writes
// EP_DMA_KICK to Interrupt Line, and the endpoint program (ep_dma.h) writes a
// block to the RC's memory and reads it back. The RC holds its UpdateFCs for
// 10 us (under the 30 us limit, 2.6.1.2): the endpoint runs out of posted
// credits and must wait. The checker judges (TL.REQ-*, FC.EP-CREDITS).
//-------------------------------------------------------------

static void Dma(pcieModelClass *pcie)
{
    uint32_t il   = CfgSweepRd(pcie, 0x3c);
    int      tick = (GetLinkSpeed(rc_node) == 2) ? 2 : 4;
    FaultMark("the RC holds its UpdateFCs; the endpoint's DMA");
    HoldFc(10000 / tick, rc_node);
    stim[S_FC_HOLD]++;
    CfgSweepWr(pcie, 0x3c, (il & ~0xffu) | EP_DMA_KICK);

    unsigned reqs = 0, left = 1;
    uint64_t t0   = GetLinkTimeNs(rc_node);
    while ((reqs < 2 * EP_DMA_N || left) && GetLinkTimeNs(rc_node) < t0 + 300000)
    {
        LinkIdle(100, rc_node);
        ChkEpDma(&reqs, &left);
    }
    stim[S_EP_REQ] += reqs;
    if (reqs < 2 * EP_DMA_N || left)
    {
        VPrint("**** ERROR: RC model: the endpoint's DMA: %u request(s) seen, %u not completed\n", reqs, left);
        errors++;
    }
}

// A tag no request has outstanding; with all 32 in use, the link runs on
// until a Completion frees one
static int FreeTag(void)
{
    int tag = tx_tag++ & 0x1f;
    for (int n = 1; ChkTagLive(tag); n++)
    {
        if (n % 32 == 0)
        {
            LinkIdle(1, rc_node);
        }
        tag = tx_tag++ & 0x1f;
    }
    return tag;
}

// Wait for the reads on 'tags' (100 us at most): their Completions may come
// before, in or after a Recovery
static void WaitTags(const int *tags, const int n, const char *what)
{
    auto     live = [&]() { int k = 0; for (int i = 0; i < n; i++) { k += ChkTagLive(tags[i]); } return k; };
    uint64_t t0   = GetLinkTimeNs(rc_node);
    while (live() && GetLinkTimeNs(rc_node) < t0 + 100000)
    {
        LinkIdle(1, rc_node);
    }
    if (live())
    {
        VPrint("**** ERROR: RC model: %d read(s) across %s not completed\n", live(), what);
        errors++;
    }
}

//-------------------------------------------------------------
// Speed()
//
// Link speed changes the RC directs with 4 reads outstanding: to Gen1 and back
// (4.2.6.4: Recovery, speed_change). The endpoint stops its TLPs and DLLPs at
// its first TS1 and sends the Completions after (PL.L0-ONLY), echoes
// speed_change (PL.SPEED) and keeps its Link and Lane Numbers (PL.TS-FIELDS);
// the reads complete (TL.CPL-ALL). Only on a Gen2 link.
//-------------------------------------------------------------

static void Speed(pcieModelClass *pcie)
{
    int top = GetLinkSpeed(rc_node);
    for (int gen : {1, 2})
    {
        if (top != 2)
        {
            break;
        }
        int tags[4];
        FaultMark(gen == 1 ? "the RC changes the link to Gen1, reads outstanding" : "the RC changes the link back to Gen2, reads outstanding");
        for (int i = 0; i < 4; i++)
        {
            tags[i] = FreeTag();
            pcie->memRead(bar_base[0] + 0x10, 4, tags[i], FW_REQUESTER_ID, SEND);
        }
        ChangeLinkSpeed(gen, rc_node);
        LinkIdle(1, rc_node);                   // the Recovery
        stim[S_SPEED]++;
        WaitTags(tags, 4, "the speed change");
        if (GetLinkSpeed(rc_node) != gen)
        {
            VPrint("**** ERROR: RC model: the link at Gen%d after the change to Gen%d\n", GetLinkSpeed(rc_node), gen);
            errors++;
        }
    }
}

//-------------------------------------------------------------
// Retrain()
//
// Recovery at random points, RETRAIN_COUNT times (4.2.6.5: directed): a few
// reads and writes, a random gap, then a Recovery without a speed change or,
// when both sides advertise Gen2, mostly with one, to a random rate. The
// endpoint's Completions and Acks may be on the wire, queued or not yet due;
// the reads complete (TL.CPL-ALL), and the PL. rules judge the Recovery.
//-------------------------------------------------------------

#define RETRAIN_COUNT          8
#define RETRAIN_GAP_MAX_TICKS  400     // up to 0.8 us at Gen2: past the Completions

static void Retrain(pcieModelClass *pcie)
{
    bool g2 = (GetLtssmRates(rc_node) & pcie->getTS(0).datarate & TS_DATA_RATE_GEN2) == TS_DATA_RATE_GEN2;
    for (int r = 0; r < RETRAIN_COUNT; r++)
    {
        int  gen = (g2 && Rand(3)) ? 1 + Rand(2) : 0;   // 0: no speed change
        int  n   = 1 + Rand(4), tags[4];
        char what[96];
        snprintf(what, sizeof what, "Recovery%s, %d read(s) outstanding",
                 gen == 1 ? " to Gen1" : gen == 2 ? " to Gen2" : "", n);
        FaultMark(what);
        for (int i = 0; i < n; i++)
        {
            tags[i] = FreeTag();
            pcie->memRead(bar_base[0] + 0x10, 4, tags[i], FW_REQUESTER_ID, SEND);
            if (Rand(2))
            {
                PktData_t buf[4] = {(PktData_t)Rand(256), (PktData_t)Rand(256), (PktData_t)Rand(256), (PktData_t)Rand(256)};
                pcie->memWrite(bar_base[0] + 0x10, buf, 4, 0, FW_REQUESTER_ID, SEND);
            }
        }
        LinkIdle(Rand(RETRAIN_GAP_MAX_TICKS), rc_node);
        if (gen)
        {
            ChangeLinkSpeed(gen, rc_node);
        }
        else
        {
            RetrainLink(rc_node);
        }
        LinkIdle(1, rc_node);                   // the Recovery
        stim[gen ? S_RETRAIN_SPEED : S_RETRAIN]++;
        WaitTags(tags, n, what);
        if (gen && GetLinkSpeed(rc_node) != gen)
        {
            VPrint("**** ERROR: RC model: the link at Gen%d after the change to Gen%d\n", GetLinkSpeed(rc_node), gen);
            errors++;
        }
    }
    if (g2 && GetLinkSpeed(rc_node) != 2)
    {
        ChangeLinkSpeed(2, rc_node);            // back to Gen2
        LinkIdle(1, rc_node);
        if (GetLinkSpeed(rc_node) != 2)
        {
            VPrint("**** ERROR: RC model: the link at Gen%d after the change back to Gen2\n", GetLinkSpeed(rc_node));
            errors++;
        }
    }
}

//-------------------------------------------------------------
// Mix()
//
// The tester (not A7RC=1): the BARs filled, then MIX_COUNT memory writes and reads at random to the
// placed BARs (their first EP_SCRATCH_BYTES): 1 byte to Max_Payload_Size
// (writes) or Max_Read_Request_Size (reads), any byte address, so partial
// Byte Enables, some ending at a 4 KB boundary, none crossing it (2.2.7).
// Paced at random: back to back, after a gap, or in bursts; random free tags, up to
// MIX_MAX_OUT reads outstanding (2.2.6.3). The checker judges the answers:
// TL.ORDER the data, TL.CPL-FIELDS the splits.
//-------------------------------------------------------------

#define MIX_COUNT             64
#define MIX_MAX_OUT           32              // tags 0-31: Extended Tag Field off
#define MIX_GAP_MAX_TICKS     500             // up to 2 us at Gen1
#define MIX_MPS_BYTES         128             // the RC leaves Device Control at its defaults
#define MIX_MRRS_BYTES        512

static void Mix(pcieModelClass *pcie)
{
    int bars[FW_NUM_BARS], nbars = 0;
    for (int i = 0; i < FW_NUM_BARS; i++)
    {
        if (bar_size[i])
        {
            bars[nbars++] = i;
        }
    }
    if (!nbars)
    {
        return;
    }

    // First fill each span with random data, Max_Payload_Size at a time: every
    // read then has data to check, and our endpoint model reads no memory it
    // never wrote (its sparse memory stops the run on that)
    for (int k = 0; k < nbars; k++)
    {
        int      b    = bars[k];
        uint32_t span = (bar_size[b] < EP_SCRATCH_BYTES) ? bar_size[b] : EP_SCRATCH_BYTES;
        for (uint32_t off = 0; off < span; off += MIX_MPS_BYTES)
        {
            PktData_t buf[MIX_MPS_BYTES];
            uint32_t  len = (span - off < MIX_MPS_BYTES) ? span - off : MIX_MPS_BYTES;
            for (uint32_t i = 0; i < len; i++)
            {
                buf[i] = Rand(256);
            }
            pcie->memWrite(bar_base[b] + off, buf, len, 0, FW_REQUESTER_ID, SEND);
            stim[S_FILL]++;
        }
    }

    // A read's tag is busy from the call until its last Completion: also while
    // the request waits in the queue (Recovery), before the checker sees it
    unsigned issued[MIX_MAX_OUT];
    for (int t = 0; t < MIX_MAX_OUT; t++)
    {
        issued[t] = ChkTagGen(t);
    }
    auto busy = [&](const int t) { return ChkTagLive(t) || ChkTagGen(t) != issued[t]; };
    auto nbusy = [&]() { int n = 0; for (int t = 0; t < MIX_MAX_OUT; t++) { n += busy(t); } return n; };

    int burst = 0;                            // requests left in a burst: mostly reads, no gaps
    for (int n = 0; n < MIX_COUNT; n++)
    {
        if (!burst && !Rand(8))
        {
            burst = 4 + Rand(9);
            stim[S_BURST]++;
        }
        int      b    = bars[Rand(nbars)];
        uint32_t span = (bar_size[b] < EP_SCRATCH_BYTES) ? bar_size[b] : EP_SCRATCH_BYTES;
        bool     wr   = burst ? !Rand(4) : Rand(2);
        uint32_t max  = wr ? MIX_MPS_BYTES : MIX_MRRS_BYTES;
        max           = (max < span) ? max : span;
        uint32_t len  = 1 + Rand(max);
        uint32_t off  = (!(span & 0xfff) && !Rand(4)) ? span - len : Rand(span - len + 1);
        uint32_t addr = bar_base[b] + off;
        if ((addr & 3) + len > max)
        {
            len = max - (addr & 3);             // the Length counts DWs from addr's DW: within MPS/MRRS (2.2.2, 7.8.4)
        }
        if ((addr & 0xfff) + len > 0x1000)
        {
            len = 0x1000 - (addr & 0xfff);      // never across 4 KB (2.2.7); spans inside size-aligned BARs: a guard
        }

        stim[wr ? S_MIX_WR : S_MIX_RD]++;
        stim[S_MULTI_DW]   += ((addr & 3) + len + 3) / 4 > 1;
        stim[S_PARTIAL_BE] += (addr & 3) || ((addr + len) & 3);
        stim[S_AT_4KB]     += !((addr + len) & 0xfff);

        if (burst)
        {
            burst--;
            stim[S_GAP_NONE]++;
        }
        else if (Rand(2))
        {
            stim[S_GAP_NONE]++;
        }
        else
        {
            LinkIdle(1 + Rand(MIX_GAP_MAX_TICKS), rc_node);
            stim[S_GAP_RANDOM]++;
        }

        // A free tag; wait for one while MIX_MAX_OUT are outstanding
        uint64_t wait_ns = GetLinkTimeNs(rc_node);
        while (nbusy() >= MIX_MAX_OUT && GetLinkTimeNs(rc_node) < wait_ns + FW_CPL_TIMEOUT_NS)
        {
            LinkIdle(1, rc_node);
        }
        if (nbusy() >= MIX_MAX_OUT)
        {
            VPrint("**** ERROR: RC model: %d requests still outstanding, no tag free; the mix stops\n", nbusy());
            errors++;
            return;
        }
        int tag = Rand(MIX_MAX_OUT);
        for (int k = 0; k < MIX_MAX_OUT && busy(tag); k++)
        {
            tag = (tag + 1) % MIX_MAX_OUT;      // the next free one
        }

        if (wr)
        {
            PktData_t buf[MIX_MPS_BYTES];
            for (uint32_t i = 0; i < len; i++)
            {
                buf[i] = Rand(256);
            }
            pcie->memWrite(addr, buf, len, tag, FW_REQUESTER_ID, SEND);
        }
        else
        {
            issued[tag]++;
            pcie->memRead(addr, len, tag, FW_REQUESTER_ID, SEND);
        }
        stim[S_OUT_MAX] = (ChkOutstanding() > (int)stim[S_OUT_MAX]) ? ChkOutstanding() : stim[S_OUT_MAX];
    }

    // every read completed, also those still queued for credits at the loop's end
    uint64_t sent_ns = GetLinkTimeNs(rc_node);
    while (nbusy() && GetLinkTimeNs(rc_node) < sent_ns + FW_CPL_TIMEOUT_NS)
    {
        LinkIdle(1, rc_node);
    }
    if (nbusy())
    {
        VPrint("**** ERROR: RC model: the mix ends with %d read(s) never completed\n", nbusy());
        errors++;
    }
}
#endif

//-------------------------------------------------------------
// VUserMain2()
//-------------------------------------------------------------

extern "C" void VUserMain2(int node)
{
    rc_node = node;

    VProc*          vp2  = new VProc(node);
    pcieModelClass* pcie = new pcieModelClass(node);

    pcie->initialisePcie(VUserInput_2, NULL);
    ChkInit(node);    // every TLP and DLLP checked against the spec (a7rc_check.cpp)
    VPrint("===> RC model: %s\n", A7RC_LITERAL ? "literal (A7RC=1): only what the lab RC sends"
                                               : "tester: the lab RC's sequence, then stimulus beyond it");

    const char *seed_env = getenv("A7RC_SEED");
    uint32_t    seed     = seed_env ? strtoul(seed_env, NULL, 10) : 1;
    rng.seed(seed);
    VPrint("===> RC model: seed %u (make SEED=N)\n", seed);

    // Electrically idle until Detect is done, as the Artix-7 RC; a PIPE link,
    // as the endpoint (VUserMain1.cpp), or the serial line (make IFACE=serial)
    vp2->write(LINK_STATE, 0xffff);
#ifndef SIM_SERIAL
    pcie->configurePcie(CONFIG_DISABLE_8B10B);
    pcie->configurePcie(CONFIG_DISABLE_SCRAMBLING);
#endif

    // The Artix-7 RC profile
    SetLinkProfile(LTSSM_PROFILE_ARTIX7_RC, node);
    pcie->configurePcie(CONFIG_LTSSM_POLL_ACTIVE_TX_COUNT, A7RC_POLL_ACTIVE_TS1);
    pcie->configurePcie(CONFIG_LTSSM_N_FTS,                A7RC_N_FTS);
    pcie->configurePcie(CONFIG_LTSSM_TS_CTL,               A7RC_TS_CTL);
    pcie->configurePcie(CONFIG_ENABLE_SKIPS,               A7RC_SKP_INTERVAL);
    pcie->configurePcie(CONFIG_ENABLE_ACK,                 A7RC_ACK_TICKS);
    pcie->configurePcie(CONFIG_POST_HDR_CR,                A7RC_P_HDR_CR);
    pcie->configurePcie(CONFIG_POST_DATA_CR,               A7RC_P_DATA_CR);
    pcie->configurePcie(CONFIG_NONPOST_HDR_CR,             A7RC_NP_HDR_CR);
    pcie->configurePcie(CONFIG_NONPOST_DATA_CR,            A7RC_NP_DATA_CR);

    pcie->pcieSeed(node);
#if !A7RC_LITERAL && !defined(HM_PROBE)
    // The tester: TS counts and idle times varied within the spec, from the seed
    pcie->pcieSeed(seed * 2654435761u + node);
    SetLtssmVary(A7RC_VARY, node);
#endif

    // Lane reversal: the Artix-7 RC does not reverse its lanes; a reversed x4
    // link forms only with an endpoint that does (make SIM_LANE_REVERSE=1). The
    // tester may (make SIM_LANE_REV=rc or both)
    SetLaneReversal(LANE_REV_RC && !A7RC_LITERAL, node);

    unsigned lanes;
    vp2->read(LANESADDR, &lanes);   // the wrapper's Lanes: make SIM_LANES
    if (lanes == 4)
    {
        a7rc_ack_ticks = A7RC_ACK_TICKS_X4;
        pcie->configurePcie(CONFIG_ENABLE_ACK, a7rc_ack_ticks);
    }

    do
    {
        pcie->sendOs(IDL);
    }
    while (InReset(vp2));

    // Train the link, x1 or x4 at Gen1, advertising the rates up to make
    // SIM_RC_GEN (RC_GEN), then bring up flow control a little later, as the
    // Artix-7 RC does
    InitLinkGen(lanes, (RC_GEN == 2) ? TS_DATA_RATE_GEN2 : TS_DATA_RATE_GEN1, node);
    l0_ns = GetLinkTimeNs(node);
    LinkIdle(A7RC_CFG_IDLE_TICKS, node);
    pcie->sendOs(SKP);    // in Configuration.Idle, 280 ns after the last TS2 starts, as the Artix-7 RC
    LinkIdle(A7RC_INITFC_TICKS, node);
    pcie->initFc();

#if RC_GEN == 2
    // Then, when the endpoint advertised Gen2 too, change the link to Gen2
    // (Recovery with the speed change bit) right after InitFC2, as the
    // Artix-7 RC does: 1.95 us after L0. A Gen1 endpoint stays at Gen1
    if ((pcie->getTS(0).datarate & TS_DATA_RATE_GEN2) == TS_DATA_RATE_GEN2)
    {
        ChangeLinkSpeed(2, node);
        LinkIdle(1, node);
        Check("link speed", GetLinkSpeed(node), 2);
    }
#endif

    // ---- The firmware ----
    uint32_t result;

    try
    {
        result = Firmware(pcie);
    }
    catch (const Halt &h)
    {
        result = h.marker;
    }
    stim[S_FW] = tx_tag;

#if !A7RC_LITERAL
    if (result == RESULT_PASS)
    {
        CfgSweep(pcie);
        Stress(pcie, FW_WINDOW_BASE);    // BAR0, as Firmware() checked
        Mix(pcie);
#ifndef HM_PROBE                         // make HM_PROBE=1 RC=model: the endpoint puts the faults
        Unsupported(pcie);
        Odd(pcie);
        Faults(pcie);
#ifndef A7RC_NO_DMA                      // another project's endpoint without DMA
        Dma(pcie);
#endif
        Speed(pcie);
        Retrain(pcie);
#endif
    }
#endif

#ifdef SPEED_TEST
    // make SIM_SPEED_TEST=1: the endpoint took the link to Gen1 and back while
    // the firmware ran (VUserMain1.cpp); a Gen1 RC (SIM_RC_GEN=1) kept it at Gen1
    Check("link speed after the speed test", (gen1_seen << 4) | GetLinkSpeed(node), (RC_GEN == 2) ? 0x12 : 0x11);
#endif

    // Every credit back (FC.RETURN): an UpdateFC at least every 30 us, +50% (2.6.1.2)
    uint64_t end_ns = GetLinkTimeNs(node);
    while (!ChkCreditsReturned() && GetLinkTimeNs(node) < end_ns + 45000)
    {
        LinkIdle(1, node);
    }

    LinkIdle(a7rc_ack_ticks + 100, node);    // sends the last Ack, takes the endpoint's

    // ---- Result ----
    VPrint("\n===> Stimulus: kind                              count\n");
    for (int k = 0; k < S_NUM; k++)
    {
        VPrint("===>   %-34s %6u\n", stim_name[k], stim[k]);
    }
#ifdef HM_PROBE
    int tester_errors = errors;
#endif
    errors += ChkReport();
#ifdef HM_PROBE
    // make HM_PROBE=1 RC=model: the endpoint's own 4 faults are the checker's only
    // hits, one each (DL.NAK, DL.LCRC, DL.REPLAY for its duplicate, DL.DLLP-CRC)
    bool probe_ok = tester_errors == 0 && errors == 4 && ChkFails("DL.NAK") == 1 && ChkFails("DL.LCRC") == 1 &&
                    ChkFails("DL.REPLAY") == 1 && ChkFails("DL.DLLP-CRC") == 1;
    VPrint("===> HM PROBE: %d checker hits, %s\n", errors,
           probe_ok ? "the endpoint's 4 faults, as expected" : "expected the endpoint's 4 faults only");
    errors = probe_ok ? 0 : errors;
#endif
    VPrint("===> RC model: firmware result 0x%08x\n", result);
    if (errors || result != RESULT_PASS)
    {
        VPrint("\n===> RC MODEL RESULT: FAIL, %d errors\n\n", errors);
    }
    else
    {
        VPrint("\n===> RC MODEL RESULT: PASS\n\n");
    }

    // PVH_FATAL ends the run through tb.Fatal's $fatal, so make exits non-zero
    vp2->write((errors || result != RESULT_PASS) ? PVH_FATAL : PVH_FINISH, 0, 0);
}
