// SPDX-FileCopyrightText: 2026 Chili.CHIPS
//
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// hm_probe.h: the RC's own Data Link behaviour, measured from the endpoint
// (make HM_PROBE=1, x1): the hard macro (RC=hm), or the RC model (RC=model)
// to compare. Included by VUserMain1.cpp.
//
// Once the firmware has assigned BAR0, the endpoint program, one fault at a
// time, against the RC's TLPs (RC=hm: the HM_PROBE firmware adds 16
// write/read pairs):
//   1. Naks one of its TLPs: where and when does it replay (3.5.2.1)?
//   2. holds its Acks: when does its replay timer send a TLP again?
//   3. sends a Completion with a bad LCRC: its Nak, and the sequence number
//   4. sends a Completion again after its Ack: is it Acked (3.5.3.1)?
//   5. sends a DLLP with a bad CRC (the run must still pass)
//   6. retrains the link (Recovery, no speed change), then on a Gen2 link
//   7. changes it to Gen1 and 8. back to Gen2: the RC's TS1, TS2 and EIOS
//      in each Recovery, its electrical idle, and the Recovery's length
// and, all along, the RC's Ack latency for the endpoint's TLPs, from
// the oldest TLP an Ack covers. Each result is a line "HM PROBE: ...", with
// the speed and a note if the link went through Recovery in the phase.
//==========================================================================

#ifndef HM_PROBE_H
#define HM_PROBE_H

#define HM_PROBE_PHASE_NS  200000          // a phase not seen by then: reported, its fault cleared, skipped

static int      hmp_node       = 1;
static int      hmp_phase      = 0;
static uint64_t hmp_phase_ns   = 0;
static uint64_t hmp_phase_rec  = 0;        // GetLinkRecoveryNs() at the phase's start
static uint64_t hmp_rx_ns[4096];           // the hard macro's TLPs: when each last arrived (its end)
static bool     hmp_rx_seen[4096];
static uint64_t hmp_tx_ns[4096];           // the endpoint's TLPs: when each last went out (its end)
static int      hmp_tx_last    = -1;       // ... the newest
static int      hmp_hm_acked   = -1;       // the hard macro's last Ack (or Nak) sequence number, when
static uint64_t hmp_hm_ack_ns  = 0;
static int      hmp_hm_nak     = -1;       // ... its last Nak, when
static uint64_t hmp_hm_nak_ns  = 0;
static uint64_t hmp_ep_nak_ns  = 0;        // the endpoint's own Nak, and Ack, when sent
static uint64_t hmp_ep_ack_ns  = 0;
static int      hmp_rpl_seq    = -1;       // a TLP of the hard macro's sent again: which, when it began,
static uint64_t hmp_rpl_ns     = 0;        // and the timer: from the later of its first end and the
static uint64_t hmp_rpl_timer  = 0;        // endpoint's last Ack, to the replay's start
static uint64_t hmp_ack_min    = ~0ull, hmp_ack_max = 0;   // its Ack latency for the endpoint's TLPs
static int      hmp_ack_n      = 0;
static int      hmp_dup_seq    = -1;       // the endpoint's TLP sent again after its Ack, when
static uint64_t hmp_dup_ns     = 0;
static int      hmp_bad_seq    = -2;       // the endpoint's TLP sent with a bad LCRC (-2: the next new one)
static uint64_t hmp_bad_ns     = 0;
static bool     hmp_rec_on     = false;    // a Recovery measured: the RC's ordered sets in it,
static unsigned hmp_rec_ts1    = 0, hmp_rec_ts2 = 0, hmp_rec_eios = 0;
static uint64_t hmp_rec_eios_ns = 0;       // its last EIOS, and its first TS1/TS2 after it
static uint64_t hmp_rec_back_ns = 0;

// A packet's time on the wire: its symbols, at the speed now
static uint64_t HmpDuration(const pPkt_t pkt)
{
    int n = 0;
    while (pkt->data[n] != PKT_TERMINATION)
    {
        n++;
    }
    return (uint64_t)n * ((GetLinkSpeed(hmp_node) == 2) ? 2 : 4);
}

static bool HmpNotBehind(const int a, const int b)
{
    return ((a - b) & 0xfff) < 2048;
}

static void HmProbeMon(const pPkt_t pkt, const int rx, const int edb, void *usrptr)
{
    uint64_t now = GetLinkTimeNs(hmp_node);
    if (pkt->seq == DLLP_SEQ_ID)
    {
        int type = pkt->data[1] & 0xff, seq = ((pkt->data[3] & 0xf) << 8) | (pkt->data[4] & 0xff);
        if (rx && (type == DL_ACK || type == DL_NAK))
        {
            // the RC's Ack latency: from the oldest TLP the Ack covers
            int oldest = (hmp_hm_acked + 1) & 0xfff;
            if (hmp_tx_last >= 0 && seq != hmp_hm_acked && HmpNotBehind(seq, oldest) && hmp_tx_ns[oldest])
            {
                uint64_t lat = now - hmp_tx_ns[oldest];
                hmp_ack_min  = (lat < hmp_ack_min) ? lat : hmp_ack_min;
                hmp_ack_max  = (lat > hmp_ack_max) ? lat : hmp_ack_max;
                hmp_ack_n++;
            }
            hmp_hm_acked  = seq;
            hmp_hm_ack_ns = now;
            if (type == DL_NAK)
            {
                hmp_hm_nak    = seq;
                hmp_hm_nak_ns = now;
            }
        }
        else if (!rx && type == DL_NAK)
        {
            hmp_ep_nak_ns = now;
        }
        else if (!rx && type == DL_ACK)
        {
            hmp_ep_ack_ns = now;
        }
        return;
    }
    int      seq = pkt->seq & 0xfff;
    uint64_t dur = HmpDuration(pkt);
    if (rx)
    {
        // sent again, after an arrival in this phase (not the end of the last one's replay)
        if (hmp_rx_seen[seq] && hmp_rpl_seq < 0 && hmp_rx_ns[seq] > hmp_phase_ns)
        {
            uint64_t from = (hmp_rx_ns[seq] > hmp_ep_ack_ns) ? hmp_rx_ns[seq] : hmp_ep_ack_ns;
            hmp_rpl_seq   = seq;
            hmp_rpl_ns    = now - dur;
            hmp_rpl_timer = hmp_rpl_ns - from;
        }
        hmp_rx_seen[seq] = true;
        hmp_rx_ns[seq]   = now;
    }
    else if (pkt->Retry == 0)                   // a new TLP (the monitor runs before Retry counts it)
    {
        hmp_tx_ns[seq] = now;
        hmp_tx_last    = seq;
        if (hmp_bad_seq == -2 && hmp_phase == 3 && now - dur >= hmp_phase_ns)
        {
            hmp_bad_seq = seq;                  // the first to start after the fault was armed
            hmp_bad_ns  = now;
        }
        if (hmp_dup_seq == -2 && hmp_phase == 4 && now - dur >= hmp_phase_ns)
        {
            hmp_dup_seq = seq;                  // FAULT_TX_DUP copies the first new TLP after arming
        }
    }
    else if (seq == hmp_dup_seq)
    {
        hmp_dup_ns = now;                       // the duplicate's end
    }
    else
    {
        hmp_tx_ns[seq] = now;                   // a replay: the Ack latency counts from it
    }
}

// The RC's ordered sets, lane 0 (SetOsMonitor())
static void HmProbeOs(int type, int lane, pTS_t ts, void *usrptr)
{
    if (!hmp_rec_on || lane != 0)
    {
        return;
    }
    hmp_rec_ts1  += (type == TS1_ID);
    hmp_rec_ts2  += (type == TS2_ID);
    if (type == IDL)
    {
        hmp_rec_eios++;
        hmp_rec_eios_ns = GetLinkTimeNs(hmp_node);
        hmp_rec_back_ns = 0;
    }
    else if ((type == TS1_ID || type == TS2_ID) && hmp_rec_eios_ns && !hmp_rec_back_ns)
    {
        hmp_rec_back_ns = GetLinkTimeNs(hmp_node);
    }
}

// One Recovery from the endpoint: a retrain (gen 0) or a speed change to 'gen'
static void HmpRecovery(const int gen, const int node)
{
    int      from = GetLinkSpeed(node);
    uint64_t t0   = GetLinkTimeNs(node);
    hmp_rec_ts1 = hmp_rec_ts2 = hmp_rec_eios = 0;
    hmp_rec_eios_ns = hmp_rec_back_ns = 0;
    hmp_rec_on  = true;
    if (gen)
    {
        ChangeLinkSpeed(gen, node);
    }
    else
    {
        RetrainLink(node);
    }
    LinkIdle(1, node);                          // the whole Recovery
    hmp_rec_on = false;
    VPrint("HM PROBE: Recovery, %s (Gen%d to Gen%d): the RC sent %u TS1, %u TS2, %u EIOS; its idle %llu ns "
           "(last EIOS to its first TS after); the Recovery %llu ns (to the endpoint's L0)\n",
           gen ? "speed change" : "retrain", from, GetLinkSpeed(node), hmp_rec_ts1, hmp_rec_ts2, hmp_rec_eios,
           (unsigned long long)(hmp_rec_back_ns ? hmp_rec_back_ns - hmp_rec_eios_ns : 0),
           (unsigned long long)(GetLtssmL0Ns(node) - t0));
}

static void HmPhase(const int next)
{
    hmp_phase     = next;
    hmp_phase_ns  = GetLinkTimeNs(hmp_node);
    hmp_phase_rec = GetLinkRecoveryNs(hmp_node);
    hmp_rpl_seq   = -1;
    hmp_hm_nak    = -1;
}

static const char *HmpNote(void)
{
    return GetLinkRecoveryNs(hmp_node) != hmp_phase_rec ? " (Recovery during the phase)" : "";
}

// From the endpoint program's idle loop
static void HmProbe(pcieModelClass *pcie, const int node)
{
    uint64_t now  = GetLinkTimeNs(node);
    bool     late = hmp_phase > 0 && now - hmp_phase_ns > HM_PROBE_PHASE_NS;
    int      gen  = GetLinkSpeed(node);

    switch (hmp_phase)
    {
    case 0:                                     // BAR0 assigned (not its sizing): the memory traffic follows
    {
        uint32_t bar0 = pcie->readConfigSpace(0x10) & 0xfffff000;
        if (bar0 && bar0 != 0xfffff000)
        {
            hmp_node = node;
            InjectFault(FAULT_RX_NAK_NEXT, node);
            HmPhase(1);
        }
        break;
    }
    case 1:                                     // 1. a Nak: the replay
        if (hmp_rpl_seq >= 0 || late)
        {
            if (hmp_rpl_seq >= 0)
                VPrint("HM PROBE: Nak (Gen%d): the RC sent TLP %d again, starting %llu ns after the endpoint's Nak%s\n",
                       gen, hmp_rpl_seq, (unsigned long long)(hmp_rpl_ns - hmp_ep_nak_ns), HmpNote());
            else
                VPrint("HM PROBE: Nak: no replay seen\n");
            ClearFaults(node);
            pcie->configurePcie(CONFIG_ENABLE_ACK, 5000);   // 2. no Ack for 5000 ticks
            HmPhase(2);
        }
        break;
    case 2:                                     // 2. Acks held: its replay timer
        if (hmp_rpl_seq >= 0 || late)
        {
            if (hmp_rpl_seq >= 0)
                VPrint("HM PROBE: replay timer (Gen%d): the RC sent TLP %d again %llu ns after the timer could start "
                       "(the table at x1, MPS 128: %d symbols, %d ns, -0%%/+100%%)%s\n", gen, hmp_rpl_seq,
                       (unsigned long long)hmp_rpl_timer, gen == 2 ? 864 : 711, gen == 2 ? 1728 : 2844, HmpNote());
            else
                VPrint("HM PROBE: replay timer: no replay seen\n");
            pcie->configurePcie(CONFIG_ENABLE_ACK, DEFAULT_ACK_RATE);
            InjectFault(FAULT_TX_BAD_LCRC, node);
            hmp_bad_seq = -2;
            HmPhase(3);
        }
        break;
    case 3:                                     // 3. a bad LCRC: its Nak
        if ((hmp_hm_nak >= 0 && hmp_bad_seq >= 0) || late)
        {
            if (hmp_hm_nak >= 0 && hmp_bad_seq >= 0)
                VPrint("HM PROBE: bad LCRC (Gen%d): the RC Naked with %d; the bad TLP was %d; %llu ns after its end%s\n",
                       gen, hmp_hm_nak, hmp_bad_seq, (unsigned long long)(hmp_hm_nak_ns - hmp_bad_ns), HmpNote());
            else
                VPrint("HM PROBE: bad LCRC: no Nak seen\n");
            ClearFaults(node);
            InjectFault(FAULT_TX_DUP, node);
            hmp_dup_seq = -2;
            HmPhase(4);
        }
        break;
    case 4:                                     // 4. a Completion again after its Ack
        if (hmp_dup_seq >= 0 && HmpNotBehind(hmp_hm_acked, hmp_dup_seq))
        {
            hmp_dup_ns    = 0;
            hmp_hm_ack_ns = 0;
            SendDuplicate(node);
            for (int n = 0; n < 2000 && !(hmp_dup_ns && hmp_hm_ack_ns > hmp_dup_ns); n++)
            {
                LinkIdle(1, node);
            }
            if (hmp_dup_ns && hmp_hm_ack_ns > hmp_dup_ns && HmpNotBehind(hmp_hm_acked, hmp_dup_seq))
                VPrint("HM PROBE: duplicate (Gen%d): the RC Acked %d, %llu ns after TLP %d's second end%s\n",
                       gen, hmp_hm_acked, (unsigned long long)(hmp_hm_ack_ns - hmp_dup_ns), hmp_dup_seq, HmpNote());
            else
                VPrint("HM PROBE: duplicate: no Ack seen\n");
            ClearFaults(node);
            InjectFault(FAULT_TX_BAD_DLLP, node);
            HmPhase(5);
        }
        else if (late)
        {
            VPrint("HM PROBE: duplicate: no Completion to copy\n");
            ClearFaults(node);
            HmPhase(5);
        }
        break;
    case 5:                                     // 5. a bad DLLP: nothing to see, the run must pass
        VPrint("HM PROBE: bad DLLP (Gen%d): sent; the RC's Ack latency for the endpoint's TLPs, from the oldest "
               "each Ack covers: %d measured, %llu to %llu ns (limit, x1 MPS 128: Gen1 237 symbols 948 ns, Gen2 288 "
               "symbols 576 ns)\n", gen, hmp_ack_n, (unsigned long long)(hmp_ack_n ? hmp_ack_min : 0),
               (unsigned long long)hmp_ack_max);
        HmPhase(6);
        break;
    case 6:                                     // 6. a retrain
        HmpRecovery(0, node);
        HmPhase(gen == 2 ? 7 : 9);
        break;
    case 7:                                     // 7. to Gen1, 8. back to Gen2
    case 8:
        HmpRecovery(hmp_phase == 7 ? 1 : 2, node);
        HmPhase(hmp_phase + 1);
        break;
    default:
        break;
    }
}

#endif
