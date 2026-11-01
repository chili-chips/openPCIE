// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// ep_dma.h: the endpoint's own requests, for the RC model's tester (Dma() in
// VUserMain2.cpp). Included by the endpoint program, VUserMain1.cpp, here and
// in openCologne-PCIE (the same file in both).
//
// When the RC writes EP_DMA_KICK to Interrupt Line, and Bus Master Enable is
// set (7.5.1.1): EP_DMA_N writes of EP_DMA_BYTES to the RC's memory, then a
// read of each, the data compared. A mismatch, or a read not completed,
// stops the run. The RC holds its UpdateFCs meanwhile (HoldFc()), so the
// writes wait for credits (2.6.1).
//==========================================================================

#ifndef EP_DMA_H
#define EP_DMA_H

#define EP_DMA_KICK   0xd5
#define EP_DMA_BASE   0x00100000u
#define EP_DMA_N      48
#define EP_DMA_BYTES  64

#ifndef EP_DMA_CONSTANTS_ONLY                  // the tester takes the numbers only

static PktData_t ep_dma_buf[EP_DMA_N][EP_DMA_BYTES];
static int       ep_dma_blk[32], ep_dma_left[32];   // per tag: the block, bytes still to come
static int       ep_dma_errors = 0, ep_dma_lost = 0;
static bool      ep_dma_done   = false;

// A Completion to a DMA read: compared, true when taken (from VUserInput_1)
static bool EpDmaCpl(const pPkt_t pkt)
{
    if (pkt->seq == DLLP_SEQ_ID || (GET_TLP_TYPE(pkt->data) & TLP_TYPE_MASK) != TL_CPLD)
    {
        return false;
    }
    int tag = GET_CPL_TAG(pkt->data) & 0x1f;
    if (ep_dma_left[tag] <= 0)
    {
        return false;
    }
    int off = EP_DMA_BYTES - (int)GET_CPL_BYTECOUNT(pkt->data);
    if (off < 0)                        // a Byte Count above the block
    {
        ep_dma_errors++;
        return true;
    }
    for (int i = 0; i < pkt->ByteCount && off + i < EP_DMA_BYTES; i++)
    {
        if ((GET_TLP_PAYLOAD_PTR(pkt->data)[i] & 0xff) != ep_dma_buf[ep_dma_blk[tag]][off + i])
        {
            ep_dma_errors++;
        }
    }
    ep_dma_left[tag] -= pkt->ByteCount;
    return true;
}

// Waits 'ticks' symbol times, following the link (the program's own idle)
typedef void (*ep_idle_t)(const int ticks, const int node);

// The DMA, once asked (call from the idle loop)
static void EpDma(pcieModelClass *pcie, const int node, const ep_idle_t idle)
{
    if (ep_dma_done || (pcie->readConfigSpace(0x3c) & 0xff) != EP_DMA_KICK || !(pcie->readConfigSpace(0x04) & 4))
    {
        return;
    }
    ep_dma_done = true;

    uint32_t rid = GetCompleterId(node) & 0xfff8;
    for (int b = 0; b < EP_DMA_N; b++)
    {
        for (int i = 0; i < EP_DMA_BYTES; i++)
        {
            ep_dma_buf[b][i] = (PktData_t)((b * 37 + i * 11 + 5) & 0xff);
        }
        pcie->memWrite(EP_DMA_BASE + b * EP_DMA_BYTES, ep_dma_buf[b], EP_DMA_BYTES, 0, rid, SEND);
    }
    for (int b = 0; b < EP_DMA_N; b++)
    {
        int tag = b & 0x1f;
        for (int n = 0; ep_dma_left[tag] > 0 && n < 20000; n++)
        {
            idle(10, node);
        }
        ep_dma_lost += ep_dma_left[tag] > 0;   // its tag needed again: that read is lost
        ep_dma_blk[tag]  = b;
        ep_dma_left[tag] = EP_DMA_BYTES;
        pcie->memRead(EP_DMA_BASE + b * EP_DMA_BYTES, EP_DMA_BYTES, tag, rid, SEND);
    }
    int left = 0;
    for (int n = 0; n < 20000; n++)
    {
        left = 0;
        for (int t = 0; t < 32; t++)
        {
            left += ep_dma_left[t] > 0;
        }
        if (!left)
        {
            break;
        }
        idle(10, node);
    }
    VPrint("===> EP DMA: %d writes, %d reads of %d bytes: %d byte(s) wrong, %d read(s) not completed\n", EP_DMA_N,
           EP_DMA_N, EP_DMA_BYTES, ep_dma_errors, left + ep_dma_lost);
    if (ep_dma_errors || left || ep_dma_lost)
    {
        VPrint("**** ERROR: EP DMA failed\n");
        VWrite(PVH_FATAL, 0, 0, node);
    }
}

#endif
#endif
