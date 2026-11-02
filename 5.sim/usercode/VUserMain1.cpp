// SPDX-FileCopyrightText: 2025 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// Copyright (c) 2025 Chili.CHIPS
//--------------------------------------------------------------------------
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
//   - User test program for pcievhost endpoint at node 1
//==========================================================================

//=============================================================
// VUserMain1.cpp
//=============================================================

#include <stdio.h>
#include <stdlib.h>

#include "VProcClass.h"
#include "pcieModelClass.h"
#ifdef SIM_SWITCHED
#include "pcie_switch.h"
#endif

extern "C" {
#include "ltssm.h"
#include "ep_dma.h"
#ifdef HM_PROBE
#include "hm_probe.h"
#endif
}


// Rates the endpoint advertises: 1 or 2 (make SIM_GEN)
#ifndef EP_GEN
#define EP_GEN 1
#endif

// May reverse its lanes (make SIM_LANE_REV=ep or both)
#ifndef LANE_REV_EP
#define LANE_REV_EP 0
#endif

// make SIM_FAULT=...: the endpoint misbehaves, to drive the RC firmware's
// error paths
//   crs      the first FAULT_CRS_COUNT Configuration requests get CRS
//   nocpl    no Completion for the request after the RC's BAR0 write
//   late     the endpoint leaves electrical idle FAULT_LATE_TICKS after reset
//   linkdown the endpoint goes electrically idle for good at the RC's BAR0 write
#define FAULT_CRS_COUNT       3
#define FAULT_LATE_TICKS      50000   // 200 us at Gen1: after the firmware's first request

#if defined(SIM_FAULT_CRS) || defined(SIM_FAULT_NOCPL)
static pcieModelClass *fault_pcie = NULL;     // set by VUserMain1()
static int             fault_left = FAULT_CRS_COUNT;
static bool            fault_on   = true;     // until the config space is handed back
#endif

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
// VUserInput_1()
//
// Consumes the unhandled input Packets
//-------------------------------------------------------------

#if defined(SIM_FAULT_CRS) || defined(SIM_FAULT_NOCPL)
//-------------------------------------------------------------
// FaultInput()
//
// make SIM_FAULT=crs/nocpl: while the model's own config space is off
// (CONFIG_DISABLE_MEM), Configuration requests come here. Answers CRS,
// or none; then hands the config space back. True when it took the packet.
//-------------------------------------------------------------

static bool FaultInput(pPkt_t pkt)
{
    int type = GET_TLP_TYPE(pkt->data);
    int tag  = GET_TLP_TAG(pkt->data);

    // After the fault, pcieVHost answers everything itself, and still hands
    // unsupported TLPs here (a Cfg to a function it lacks): leave them alone
    if (!fault_on || (type != TL_CFGRD0 && type != TL_CFGWR0))
    {
        return false;
    }

#ifdef SIM_FAULT_CRS
    VPrint("===> EP fault: CRS for tag %d\n", tag);
    fault_pcie->partCompletionDelay(0, NULL, CPL_CRS, 0, 0, 0, 0, tag, 0, GET_TLP_RID(pkt->data), false, false, true);
    if (--fault_left == 0)
#else
    VPrint("===> EP fault: no Completion for tag %d\n", tag);
#endif
    {
        fault_pcie->configurePcie(CONFIG_ENABLE_MEM);
        fault_on = false;
    }

    DISCARD_PACKET(pkt);
    return true;
}
#endif

static void VUserInput_1(pPkt_t pkt, int status, void* usrptr)
{
    int idx;

#if defined(SIM_FAULT_CRS) || defined(SIM_FAULT_NOCPL)
    if (pkt->seq != DLLP_SEQ_ID && FaultInput(pkt))
    {
        return;
    }
#endif

    if (pkt->seq == DLLP_SEQ_ID)
    {
        DebugVPrint("---> VUserInput_1 received DLLP\n");
    }
    else
    {
        DebugVPrint("---> VUserInput_1 received TLP sequence %d of %d bytes at %d\n", pkt->seq, GET_TLP_LENGTH(pkt->data), pkt->TimeStamp);
    }

    EpDmaCpl(pkt);                              // the DMA's read data, compared

    // Once packet is finished with, the allocated space *must* be freed.
    // All input packets have their own memory space to avoid overwrites
    // which shared buffers.
    DISCARD_PACKET(pkt);
}

//-------------------------------------------------------------
// ConfigureType0PcieCfg()
//
// Configure the configuartion space buffer for an endpoint
// (type 0) PCIe configuration space with PCI compatible header,
// PCIe capabilities, MSI capabilities and Power managment
// capabilities, along with some default values.
//
//-------------------------------------------------------------

static unsigned ep_lanes = 1;   // the wrapper's lanes (VUserMain1())

static void ConfigureType0PcieCfg (pcieModelClass *pcie, const int node)
{
    unsigned        next_cap_ptr = 0;

    // -------------------------------------
    // PPCI compatible header
    // -------------------------------------

    cfg_spc_type0_t     type0;
    cfg_spc_type0_t     type0_mask;
    cfg_spc_pcie_caps_t pcie_caps;
    cfg_spc_pcie_caps_t pcie_caps_mask;
    cfg_spc_msi_caps_t  msi_caps;
    cfg_spc_msi_caps_t  msi_caps_mask;
    pwr_mgmnt_caps_t    pwr_mgmnt_caps;
    pwr_mgmnt_caps_t    pwr_mgmnt_caps_mask;

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PCI_HDR_SIZE_BYTES/4); idx++)
    {
        type0.words[idx]      = 0x00000000;
        type0_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PCI_HDR_SIZE_BYTES;

    // Construct PCI compatible structure value, and set writable bits where appropriate
    type0.type0_struct.vendor_id               = 0x14fc;
    type0.type0_struct.device_id               = 0x0002;
    type0.type0_struct.command                 = 0x0006;     type0_mask.type0_struct.command = 0xfab8;
    type0.type0_struct.status                  = 0x0010;
    type0.type0_struct.revision_id             = 0x01;
    type0.type0_struct.prog_if                 = 0x00;       // don't care
    type0.type0_struct.subclass                = 0x80;       // other
    type0.type0_struct.class_code              = 0x02;       // network controller
    type0.type0_struct.cache_line_size         = 0x00;       type0_mask.type0_struct.cache_line_size         = 0x00;
    type0.type0_struct.bar[0]                  = 0x00000008; type0_mask.type0_struct.bar[0]                  = 0x00000fff; // 32-bit, prefetchable, 4K
    type0.type0_struct.bar[1]                  = 0x00000008; type0_mask.type0_struct.bar[1]                  = 0x000003ff; // 32-bit, prefetchable, 1K
    type0.type0_struct.bar[2]                  = 0x0000000c; type0_mask.type0_struct.bar[2]                  = 0xffffffff; // 64-bit, prefetchable
    type0.type0_struct.bar[3]                  = 0x00000000; type0_mask.type0_struct.bar[3]                  = 0x00000001; // 8G
    type0.type0_struct.bar[4]                  = 0x00000000; type0_mask.type0_struct.bar[4]                  = 0xffffffff; // Disabled
    type0.type0_struct.bar[5]                  = 0x00000000; type0_mask.type0_struct.bar[5]                  = 0xffffffff; // Disabled
    type0.type0_struct.expansion_rom_base_addr = 0x00000000; type0_mask.type0_struct.expansion_rom_base_addr = 0x000007fe;
    type0.type0_struct.interrupt_line          = 0x00;       type0_mask.type0_struct.interrupt_line          = 0x00;       // RW (7.5.1): the RC model's DMA trigger (ep_dma.h)
    type0.type0_struct.capabilities_ptr        = next_cap_ptr;

    // Update config space and mask with values
    for (int idx = 0; idx < (CFG_PCI_HDR_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PCI_HDR_SIZE_BYTES + idx*4, type0.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PCI_HDR_SIZE_BYTES + idx*4, type0_mask.words[idx]);
    }

    // -------------------------------------
    // PCIe capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PCIE_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie_caps.words[idx]      = 0x00000000;
        pcie_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PCIE_CAPS_SIZE_BYTES;

    pcie_caps.pcie_caps_struct.cap_id         = 0x10;
    pcie_caps.pcie_caps_struct.next_cap_ptr   = next_cap_ptr;
    pcie_caps.pcie_caps_struct.pcie_cap_reg   = 0x0002;       // version 2, PCI Express Endpoint (openPCIE; was 0)
    pcie_caps.pcie_caps_struct.device_caps    = 0x00008001;   // max payload = 256 bytes; Role-Based Error Reporting (7.8.3)
    pcie_caps.pcie_caps_struct.device_control = 0x2810;       pcie_caps_mask.pcie_caps_struct.device_control = 0x0000;
    pcie_caps.pcie_caps_struct.link_caps      = 0x0003fc00 | (ep_lanes << 4) | EP_GEN;   // Gen EP_GEN, x'lanes' (openPCIE; was Gen2 x1)
    pcie_caps.pcie_caps_struct.link_control   = 0x0000;       pcie_caps_mask.pcie_caps_struct.link_control   = 0xf004;
    pcie_caps.pcie_caps_struct.link_status    = 0x0011;       // Gen1 x1 until trained, then UpdateLinkStatus() (openPCIE; was 0x0091, x9)
    pcie_caps.pcie_caps_struct.link_control2  = 0x0002;       pcie_caps_mask.pcie_caps_struct.link_control2  = 0xe06f;

    for (int idx = 0; idx < (CFG_PCIE_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + idx*4, pcie_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + idx*4, pcie_caps_mask.words[idx]);
    }

    // RW1C bits (7.4), read-only in the masks above: Status 15:11 and 8,
    // Device Status 3:0 (openPCIE; upstream's config space had no RW1C)
    WriteConfigSpaceW1c(0x04, 0xf9000000, node);
    WriteConfigSpaceW1c(next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + 8, 0x000f0000, node);

    // ... and set by the model on an Unsupported Request: Device Status UR
    // Detected, and its severity bit (the model reads Role-Based Error Reporting)
    SetUrDetectedReg(next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + 8, 0x00080000, node);

    // -------------------------------------
    // MSI capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_MSI_CAPS_SIZE_BYTES/4); idx++)
    {
        msi_caps.words[idx]      = 0x00000000;
        msi_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_MSI_CAPS_SIZE_BYTES;

    msi_caps.msi_caps_struct.cap_id           = 0x05;
    msi_caps.msi_caps_struct.next_cap_ptr     = next_cap_ptr;
    msi_caps.msi_caps_struct.mess_control     = 0x0080;     msi_caps_mask.msi_caps_struct.mess_control = 0xff8e;
    msi_caps.msi_caps_struct.mess_addr_lo     = 0x00000000; msi_caps_mask.msi_caps_struct.mess_addr_lo = 0x00000003;
    msi_caps.msi_caps_struct.mess_addr_hi     = 0x00000000; msi_caps_mask.msi_caps_struct.mess_addr_hi = 0x00000000;
    msi_caps.msi_caps_struct.mess_data        = 0x0000;     msi_caps_mask.msi_caps_struct.mess_data    = 0x0000;
    msi_caps.msi_caps_struct.mask             = 0x00000000; msi_caps_mask.msi_caps_struct.mask         = 0x00000000;
    msi_caps.msi_caps_struct.pending          = 0x00000000;

    for (int idx = 0; idx < (CFG_MSI_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_MSI_CAPS_SIZE_BYTES + idx*4, msi_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_MSI_CAPS_SIZE_BYTES + idx*4, msi_caps_mask.words[idx]);
    }

    // -------------------------------------
    // Power managment capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PWR_MGMT_CAPS_SIZE_BYTES/4); idx++)
    {
        pwr_mgmnt_caps.words[idx]      = 0x00000000;
        pwr_mgmnt_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PWR_MGMT_CAPS_SIZE_BYTES;

    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.cap_id                   = 0x01;
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.next_cap_ptr             = 0x00; // last capability
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.pwr_mgmnt_caps           = 0x0003;
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.pwr_mgmnt_control_status = 0x0008;  pwr_mgmnt_caps_mask.pwr_mgmnt_caps_struct.pwr_mgmnt_control_status = 0xe0fc;

    for (int idx = 0; idx < (CFG_PWR_MGMT_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PWR_MGMT_CAPS_SIZE_BYTES + idx*4, pwr_mgmnt_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PWR_MGMT_CAPS_SIZE_BYTES + idx*4, pwr_mgmnt_caps_mask.words[idx]);
    }
}

//-------------------------------------------------------------
// UpdateLinkStatus()
//
// Link Status follows the link: the current speed and the negotiated
// width, x'lanes' (openPCIE; upstream's example kept a fixed value)
//-------------------------------------------------------------

#define CFG_LINK_STATUS_DW    (CFG_PCI_HDR_SIZE_BYTES + 0x10)   // PCIe Capability, right after the header

static void UpdateLinkStatus(pcieModelClass *pcie, const int lanes, const int node)
{
#ifndef SIM_SWITCHED                            // the switch keeps its own config space (pcie_switch.cpp)
    uint32_t dw  = pcie->readConfigSpace(CFG_LINK_STATUS_DW);
    uint32_t sta = (GetLinkSpeed(node) & 0xf) | ((lanes & 0x3f) << 4);

    if ((dw >> 16) != sta)
    {
        pcie->writeConfigSpace(CFG_LINK_STATUS_DW, (dw & 0xffff) | (sta << 16));
    }
#else
    (void)pcie; (void)lanes; (void)node;
#endif
}

//-------------------------------------------------------------
// VUserMain1()
//
// Endpoint program Initialises link and FC
// before sending idles indefinitely.
//
//-------------------------------------------------------------

extern "C" void VUserMain1(int node)
{
    // Create VProc API object for this node
    VProc* vp1 = new VProc(node);
    
    // Create pcievhost API object for this node
    pcieModelClass *pcie = new pcieModelClass(node);

#ifdef SIM_SWITCHED
    // RC-switched (make VARIANT=switched): pcie_switch.cpp answers every
    // request as a switch with endpoints behind it, so the model's own memory
    // and config space are off
    pcie->initialisePcie(SwitchInput, pcie);
    pcie->configurePcie(CONFIG_DISABLE_MEM);
#else
    // Initialise PCIe VHost, with input callback function and no user pointer.
    pcie->initialisePcie(VUserInput_1, NULL);
#ifdef HM_PROBE
    SetPktMonitor(HmProbeMon, NULL, node);       // the hard macro measured (hm_probe.h)
    SetOsMonitor(HmProbeOs, NULL, node);
#endif
#endif
#if defined(SIM_FAULT_CRS) || defined(SIM_FAULT_NOCPL)
    fault_pcie = pcie;
#endif
#ifdef SIM_FAULT_CRS
    pcie->configurePcie(CONFIG_DISABLE_MEM);     // FaultInput() answers the first requests
#endif

    // Make sure the link is out of electrical idle
    vp1->write(LINK_STATE, 0);
    
    // Configure for a PIPE link. The PIPE interface carries decoded 8-bit
    // symbols with a separate K flag, so the model must not do the line coding
    // itself. On the serial line (make IFACE=serial) it does.
#ifndef SIM_SERIAL
    pcie->configurePcie(CONFIG_DISABLE_8B10B);
    pcie->configurePcie(CONFIG_DISABLE_SCRAMBLING);
#endif
    
    //Marke DUT as node 2
    // No AER, so no ECRC generation (2.7.1): none on Completions to requests
    // with ECRC either (upstream's default adds one)
    pcie->configurePcie(CONFIG_DISABLE_ECRC_CMPL);

    pcie->configurePcie(CONFIG_DISP_BCK_NODE_NUM, 2);

    DebugVPrint("VUserMain: in node %d\n", node);

    // Use node number as seed
    pcie->pcieSeed(node);

    // May reverse its lanes in Configuration (make SIM_LANE_REV=ep or both)
    SetLaneReversal(LANE_REV_EP, node);

    vp1->read(LANESADDR, &ep_lanes);

#ifdef SIM_SWITCHED
    SwitchInit();
#else
    // Construct an endpoint PCIe configuration space
    ConfigureType0PcieCfg(pcie, node);

    // CRS after reset (2.3.1): one seed in four (make SEED=N, A7RC_SEED) answers
    // its first one or two Configuration Requests with CRS: the RC retries, and
    // the checker's TL.CRS sees it
    const char *seed_env = getenv("A7RC_SEED");
    uint32_t    seed     = seed_env ? strtoul(seed_env, NULL, 10) : 1;
    SetCfgCrs((seed % 4) ? 0 : 1 + (seed / 4) % 2, node);
#endif

    // Send out idles until reset is released
    do
    {
        pcie->sendOs(IDL);
    }
    while (InReset(vp1));

#ifdef SIM_FAULT_LATE
    VPrint("===> EP fault: electrically idle for %d ticks\n", FAULT_LATE_TICKS);
    vp1->write(LINK_STATE, 0xffff);
    pcie->sendIdle(FAULT_LATE_TICKS);
    vp1->write(LINK_STATE, 0);
#endif

    // Initialise the link for as many lanes as the model has (its LinkWidth,
    // set by the PIPE model: 1 or 4, make SIM_LANES), advertising Gen1 or
    // Gen1+Gen2 (make SIM_GEN)
    unsigned lanes;
    vp1->read(LANESADDR, &lanes);
    InitLinkGen(lanes, (EP_GEN == 2) ? TS_DATA_RATE_GEN2 : TS_DATA_RATE_GEN1, node);

    // Initialise DLLP flow control.
    //
    // This has to happen promptly. The link partner reaches L0 at the physical
    // layer at about the same moment InitLink() returns here, and from then on
    // its data link layer is waiting for the InitFC1/InitFC2 exchange. If flow
    // control does not complete, the partner's LTSSM retrains the link.
    pcie->initFc();
    UpdateLinkStatus(pcie, lanes, node);

#ifdef SPEED_TEST
    // make SIM_SPEED_TEST=1: change the link down to Gen1 and back up, both
    // from this side, when the RC's BAR sizing writes BAR0, then BAR1. Each
    // change starts with that write's Completion still to go, whenever the
    // RC's firmware gets there
    const uint32_t bar0 = pcie->readConfigSpace(CFG_BAR_HDR_OFFSET);
    const uint32_t bar1 = pcie->readConfigSpace(CFG_BAR_HDR_OFFSET + 4);

    while (pcie->readConfigSpace(CFG_BAR_HDR_OFFSET) == bar0)
    {
        LinkIdle(1, node);
        UpdateLinkStatus(pcie, lanes, node);
    }
    ChangeLinkSpeed(1, node);
    while (pcie->readConfigSpace(CFG_BAR_HDR_OFFSET + 4) == bar1)
    {
        LinkIdle(1, node);
        UpdateLinkStatus(pcie, lanes, node);
    }
    ChangeLinkSpeed(2, node);
#endif

#if defined(SIM_FAULT_NOCPL) || defined(SIM_FAULT_LINKDOWN)
    // make SIM_FAULT=nocpl/linkdown: wait for the RC's BAR0 write
    const uint32_t bar0_reset = pcie->readConfigSpace(CFG_BAR_HDR_OFFSET);

    while (pcie->readConfigSpace(CFG_BAR_HDR_OFFSET) == bar0_reset)
    {
        LinkIdle(1, node);
        UpdateLinkStatus(pcie, lanes, node);
    }
#ifdef SIM_FAULT_NOCPL
    pcie->configurePcie(CONFIG_DISABLE_MEM);     // FaultInput() drops the next request
#else
    VPrint("===> EP fault: link down\n");
    vp1->write(LINK_STATE, 0xffff);
    while (true)
    {
        pcie->sendIdle(100);
    }
#endif
#endif

    // Send out idles forever, following the RC into Recovery (speed changes);
    // the DMA when the RC model's tester asks (ep_dma.h)
    while (true)
    {
        LinkIdle(100, node);
        UpdateLinkStatus(pcie, lanes, node);
#ifndef SIM_SWITCHED
        EpDma(pcie, node, LinkIdle);
#endif
#ifdef HM_PROBE
        HmProbe(pcie, node);
#endif
    }
}

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
