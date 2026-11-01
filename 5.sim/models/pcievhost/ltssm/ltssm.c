//=============================================================
//
// Copyright (c) 2016 - 2026 Simon Southwell. All rights reserved.
//
// Date: 20th Sep 2016
//
// This file is part of the pcieVHost package.
//
// pcieVHost is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// pcieVHost is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with pcieVHost. If not, see <http://www.gnu.org/licenses/>.
//
//=============================================================
//
// Implements a PCIe LTSSM function. NB. IT IS NOT COMPLETE,
// and is meant only to be able to power up a link to L0. With
// LTSSM_ABBREVIATED defined, sequence is shortened in various
// places and timeouts reduced.
//
//=============================================================

// -------------------------------------------------------------------------
// INCLUDES
// -------------------------------------------------------------------------

#include "ltssm.h"

// -------------------------------------------------------------------------
// DEFINES
// -------------------------------------------------------------------------

// Assume a clock rate of 500MHz unless set externally
#ifdef OSVVM_LTSSM_CLKPERIOD_NS
#define CLK_CYCLE_NS                 OSVVM_PCIE_CLKPERIOD_NS
#else
#define CLK_CYCLE_NS                 2
#endif

#define CYCLES_1US                   (1000 / CLK_CYCLE_NS)
#define CYCLES_1MS                   (1000 * CYCLES_1US)
#define CYCLES_12MS                  (12   * CYCLES_1MS)

#define DEFAULT_N_FTS                4
#define DEFAULT_TS_CTL               0
#define DEFAULT_LINKNUM              0
#define ENABLE_LANENUMS              0

#define TS_CTL_HOT_RESET             0x01
#define TS_CTL_DISABLE               0x02
#define TS_CTL_LOOPBACK              0x04
#define TS_CTL_NO_SCRAMBLE           0x08

#define LTSSM_DETECT                 0
#define LTSSM_POLLING                1
#define LTSSM_CONFIG                 2
#define LTSSM_RECOVERY               3
#define LTSSM_DISABLED               4
#define LTSSM_HOTRESET               5
#define LTSSM_LOOPBACK               6
#define LTSSM_L0                     7
#define LTSSM_L0s                    8
#define LTSSM_L1                     9
#define LTSSM_L2                     10

#ifdef LTSSM_ABBREVIATED
#define PCIE_POLLING_ACTIVE_TX_COUNT 16
#else
#define PCIE_POLLING_ACTIVE_TX_COUNT 1024
#endif

#ifdef LTSSM_ABBREVIATED
#define DEFAULT_DETECT_QUIET_TIMEOUT 1500
#else
#define DEFAULT_DETECT_QUIET_TIMEOUT CYCLES_12MS
#endif

#define DEFAULT_MAX_LINK_WIDTH       16
#define DEFAULT_MAX_LINK_WIDTH_MASK  ((1 << DEFAULT_MAX_LINK_WIDTH) - 1)

#define DEFAULT_ENABLED_TESTS        0
#define DEFAULT_FORCE_TESTS          0
#define DEFAULT_DISABLE_DISP_STATE   0

#define LTSSM_SET_MINIMUM            0

// Speed change (openPCIE). Times are in the model's symbol clock, pcieclk:
// the bit rate / 10, so 250 MHz at Gen1 and 500 MHz at Gen2.
#define SYMCLK_TICKS_PER_US(_gen)    ((_gen) == 2 ? 500 : 250)
#define TS_TICKS                     16
#define TS_DATA_RATE_5G              0x04

#define RECOVERY_RCVRLOCK            0
#define RECOVERY_RCVRCFG             1
#define RECOVERY_SPEED               2
#define RECOVERY_IDLE                3

#define RECOVERY_RCVRLOCK_TO_US      24000
#define RECOVERY_RCVRCFG_TO_US       48000
// openPCIE: a Recovery this long names itself, before the run's time limit
// (the spec's timeouts are ms; ours take a few us, a speed change included;
// a failed speed change, or an endpoint gone quiet, SIM_FAULT=linkdown, take
// longer on purpose)
#define RECOVERY_STUCK_NS            50000
#define RECOVERY_IDLE_TO_US          2000
#define RECOVERY_SPEED_IDLE_US       1      // at least 800 ns
#define RECOVERY_SPEED_FAIL_IDLE_US  6
#define SPEED_CHANGE_TS2_COUNT       32

// L0: how often LinkIdle() looks for the partner's TS1/TS2
#define L0_POLL_TICKS                16

// Artix-7 RC profile (openPCIE, 5.sim/README.md "Profile"): the TS1 the
// PCIE_2_1 root port sends once a Configuration substate's receive condition
// is met (Linkwidth.Accept, Lanenum.Accept), and the TS2 it sends in
// Configuration.Complete once it has the partner's first TS2 (16 in upstream)
#define A7RC_LINKWIDTH_ACCEPT_TS1    10
#define A7RC_LANENUM_ACCEPT_TS1      9
#define A7RC_COMPLETE_TS2            18

// The Artix-7 RC's Detect, in electrical idle (Gen1 ticks): its first TS1
// leaves 3.035 us after PERST#
#define A7RC_DETECT_QUIET_TICKS      695
#define A7RC_DETECT_ACTIVE_TICKS     60

// Symbol times from the Artix-7 RC's EIOS to its rate change in Recovery.Speed
#define A7RC_SPEED_EIOS_TICKS        16
// Recovery.Speed, other profiles: our last EIOS through the serialiser
// (IFACE=serial) before the rate changes
#define SPEED_EIOS_DRAIN_TICKS       2

// The Artix-7 RC's Recovery in a speed change, measured against our endpoint
// model. RcvrLock at the old rate: TS1 without the speed_change bit first, then
// at least so many with it, in its own speed change and in the partner's.
// RcvrLock at the new rate: at least so many TS1. Speed: from its last EIOS to
// its first TS1 at the new rate. Recalibrated 2026-10-31 against the hard
// macro (make HM_PROBE=1, phases 6-8: the endpoint's retrain and speed changes)
#define A7RC_LOCK_HOLD_OWN           1
#define A7RC_LOCK_HOLD_PARTNER       8
#define A7RC_LOCK_SC_OWN             12
#define A7RC_LOCK_SC_PARTNER         8
#define A7RC_LOCK_NEW_GEN1           24
#define A7RC_LOCK_NEW_GEN2           14
#define A7RC_SPEED_IDLE_NS_GEN1      1186
#define A7RC_SPEED_IDLE_NS_GEN2      946
// ... TS2 in RcvrCfg without a speed change, from the first received (at least 16)
#define A7RC_CFG_IDLE_TS2            18

// -------------------------------------------------------------------------
// STATICS
// -------------------------------------------------------------------------

// The array initialisations are quite gcc oriented, and may not be portable.
// However, there are other things restricting the code to linux, so use for now.

static int  ltssm_linknum            [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_LINKNUM};
static int  ltssm_ts_ctl             [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_TS_CTL};
static int  ltssm_n_fts              [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_N_FTS };
static int      ltssm_cur            [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = LTSSM_DETECT };   // openPCIE: GetLtssmState()
static uint64_t ltssm_l0_ns          [VP_MAX_NODES];                                               // openPCIE: GetLtssmL0Ns()
static int  ltssm_max_link_width     [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_MAX_LINK_WIDTH};
static int  ltssm_max_link_mask      [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_MAX_LINK_WIDTH_MASK};
static int  ltssm_detect_quiet_to    [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_DETECT_QUIET_TIMEOUT};
static int  ltssm_enable_tests       [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_ENABLED_TESTS};
static int  ltssm_force_tests        [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_FORCE_TESTS};
static int  ltssm_poll_tx_count      [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = PCIE_POLLING_ACTIVE_TX_COUNT};
static int  ltssm_disable_disp_state [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = DEFAULT_DISABLE_DISP_STATE};

static int  ltssm_tx_n_fts           [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};

// Speed change: the rates we advertise (TS_DATA_RATE_GEN1/2), the rate the
// link runs at (1 or 2), and a speed change of our own, pending
static int  ltssm_rate               [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = TS_DATA_RATE_GEN1};
static int  ltssm_cur_gen            [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 1};
static bool ltssm_speed_req          [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
// Recovery without a speed change, pending (RetrainLink())
static bool ltssm_retrain_req        [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
// TS counts and idle times varied: up to so many more than the least (SetLtssmVary())
static int  ltssm_vary               [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
static int  Vary                     (const int node);
// May reverse its lanes in Configuration.Lanenum.Wait (SetLaneReversal(), 4.2.4.9.1)
static bool ltssm_lane_rev           [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
// When the last EIOS of Recovery.Speed went out (GetLtssmEiosNs())
static uint64_t ltssm_eios_ns        [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
// An Idle timeout went to Recovery.RcvrLock since the last L0: the next goes
// to Detect (idle_to_rlock_transitioned, 4.2.6.3.6, 4.2.6.4.4)
static bool     ltssm_idle_to_rlock  [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
// When Configuration.Idle or Recovery.Idle began, 0 outside them (GetLtssmIdleNs())
static uint64_t ltssm_idle_ns        [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};

// Link time (openPCIE): ns up to the symbol clock tick ltssm_time_tick, for
// GetLinkTimeNs(). A tick is 4 ns at Gen1, 2 ns at Gen2
static uint64_t ltssm_time_ns        [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
static uint32_t ltssm_time_tick      [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};

// GetLinkRecoveryNs(): link time spent in Recovery from L0, by LinkIdle()
static uint64_t ltssm_recovery_ns    [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
// ... and when the one in progress started (ltssm_in_recovery)
static uint64_t ltssm_recovery_from  [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
static uint32_t ltssm_recovery_tfrom [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};
static bool     ltssm_in_recovery    [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
static uint32_t ltssm_recovery_ticks [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = 0};   // ... in ticks (GetCycleCount())

// Link profile (openPCIE): the link partner this node plays, SetLinkProfile()
static int  ltssm_profile            [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = LTSSM_PROFILE_EP};

static bool config_disable           [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
static bool config_loopback          [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};
static bool polling_compliance       [VP_MAX_NODES] = { [0 ... VP_MAX_NODES-1] = false};

// -------------------------------------------------------------------------
// LinkTimeUpdate()
//
// Brings the link time up to now, at the current rate. Called before every
// rate change
// -------------------------------------------------------------------------

static void LinkTimeUpdate (const int node)
{
    uint32_t now = GetCycleCount(node);

    ltssm_time_ns[node]  += (uint64_t)(now - ltssm_time_tick[node]) * 4 / ltssm_cur_gen[node];
    ltssm_time_tick[node] = now;
}

// -------------------------------------------------------------------------
// Detect()
// -------------------------------------------------------------------------

static int Detect (const int link_width, const int node)
{
    int    i = 0;
    uint32_t rcvr_idle_status;

    ltssm_max_link_width[node] = link_width;
    ltssm_max_link_mask[node]  = ((1 << ltssm_max_link_width[node])-1) & 0xffff;
    LinkTimeUpdate(node);
    ltssm_cur_gen[node]        = 1;
    VWrite(GEN2_CLK, 0, 1, node);  // the wrapper's clocks at Gen1 too, after a timeout at Gen2
    SetLaneReverse(false, node);   // lane numbers are assigned anew in Configuration
    ltssm_idle_ns[node]        = 0;
    ltssm_idle_to_rlock[node]  = false;

    // Quiet
    if (!ltssm_disable_disp_state[node]) VPrint("---> Detect Quiet (node %d)\n", node);

    // The Artix-7 RC profile: Quiet and Active take a fixed time. Without
    // electrical idle on the PIPE, the partner's receivers never look idle,
    // and the loop below would leave Quiet after one symbol
    if (ltssm_profile[node] == LTSSM_PROFILE_ARTIX7_RC)
    {
        VWrite(LINK_STATE, 0xffff, 1, node);
        SendIdle(A7RC_DETECT_QUIET_TICKS, node);
        if (!ltssm_disable_disp_state[node]) VPrint("---> Detect Active (node %d)\n", node);
        SendIdle(A7RC_DETECT_ACTIVE_TICKS, node);
        VWrite(LINK_STATE, ~ltssm_max_link_mask[node] & 0xffff, 1, node);
        return LTSSM_POLLING;
    }

    // Loop until rcvr_idle_status indicates at least one lane not idle
    do
    {
        SendIdle(1, node);
        VRead(LINK_STATE, &rcvr_idle_status, 1, node);
        DebugVPrint ("---> i=%d node=%d ltssm_detect_quiet_to[node]=%d rcvr_idle_status=0x%08x ltssm_max_link_mask[node]=0x%08x\n",
                i, node, ltssm_detect_quiet_to[node], rcvr_idle_status, ltssm_max_link_mask[node]);
    } while ((++i < ltssm_detect_quiet_to[node]) && ((rcvr_idle_status & ltssm_max_link_mask[node]) == ltssm_max_link_mask[node]));

    // Active (If no rcvr detect, assume all 16 lanes are present)
    if (!ltssm_disable_disp_state[node]) VPrint("---> Detect Active (node %d)\n", node);
    VRead(LINK_STATE, &rcvr_idle_status, 1, node);
    if (!ltssm_disable_disp_state[node]) VPrint("---> rcvr_idle_status = %x (node %d)\n", rcvr_idle_status & ltssm_max_link_mask[node], node);

    // Exit to polling
    return LTSSM_POLLING;
}

// -------------------------------------------------------------------------
// Polling()
// -------------------------------------------------------------------------

static int Polling(int *active_lanes, const int gen, const int node)
{
    uint32_t ts1_count[MAX_LINK_WIDTH] = {0}, ts2_count[MAX_LINK_WIDTH] = {0};   // openPCIE: read before the first ReadEventCount()
    TS_t ts_status;
    int i;

    // Assumes here that what is on lane 0 is common to all lanes

    // Clear TS  rx state
    ResetEventCount(TS1_ID, node);
    ResetEventCount(TS2_ID, node);

    // --- force compliance ---
    if (polling_compliance[node] == false && ((ltssm_force_tests[node] & ENABLE_COMPLIANCE) || ((ltssm_enable_tests[node] & ENABLE_COMPLIANCE) && ((PcieRand(node) % 3) == 0))))
    {
        if (!ltssm_disable_disp_state[node]) VPrint("---> Polling Compliance (node %d)\n", node);
        polling_compliance[node] = true;
        VWrite(LINK_STATE, (1 << (PcieRand(node) % ltssm_max_link_width[node])) | ~ltssm_max_link_mask[node], 1, node);

        // This is a very nasty hack, of which I am appropriately ashamed.
        // It is an open loop delay long enough for endpoint to timeout in
        // polling.active and go to compliance for a while, before we
        // continue to polling.active.
        SendIdle((28000)/4, node);
    }

    // --- Active ---
    if (!ltssm_disable_disp_state[node]) VPrint("---> Polling Active (node %d)\n", node);
    i = 0;
    VWrite(LINK_STATE, (~ltssm_max_link_mask[node]) & 0xffff, 1, node);
    do
    {
        SendTs(TS1_ID, PAD, PAD, ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
        ReadEventCount(TS1_ID, ts1_count, node);
        ReadEventCount(TS2_ID, ts2_count, node);
        ts_status = GetTS(0, node);
        if (ts1_count[0] || ts2_count[0] || i)
        {
            i++;
        }
        if ((ts1_count[0] || ts2_count[0]) && (ts_status.linknum != PAD || ts_status.lanenum != PAD))
        {
            ts1_count[0] = ts2_count[0] = 0;
        }

    } while(((ts1_count[0] < 8) && (ts2_count[0] < 8)) || (i < ltssm_poll_tx_count[node]));

    // --- Config ---
    if (!ltssm_disable_disp_state[node]) VPrint("---> Polling Config (node %d)\n", node);
    i = 0;
    ResetEventCount(TS2_ID, node);
    do
    {
        SendTs(TS2_ID, PAD, PAD, ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
        ReadEventCount(TS2_ID, ts2_count, node);
        ts_status = GetTS(0, node);
        if (ts2_count[0] || i)
        {
            i++;
        }
        if (ts2_count[0] && (ts_status.linknum != PAD || ts_status.lanenum != PAD))
        {
            ts2_count[0] = 0;
            ResetEventCount(TS2_ID, node);
        }
    } while((ts2_count[0] < 8) || (i < 16));

    *active_lanes = 0;
    for (i = 0; i < MAX_LINK_WIDTH; i++)
    {
        *active_lanes |= (ts2_count[i] ? 1 : 0) << i;
    }
    if (!ltssm_disable_disp_state[node]) VPrint("---> Active lanes = 0x%04x (node %d)\n", *active_lanes, node);

    // Exit to configuration
    return LTSSM_CONFIG;
}

// -------------------------------------------------------------------------
// Configuration()
// -------------------------------------------------------------------------

// openPCIE: one more TS (or idle symbol) in a Configuration substate; true
// once its timeout has passed (4.2.6.3): the caller goes to Detect
static bool CfgTimedOut(int *count, const int limit, const char *substate, const int node)
{
    if (++*count <= limit)
    {
        return false;
    }
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration %s: timeout, to Detect (node %d)\n", substate, node);
    return true;
}

static int Configuration(const int active_lanes, const int gen, const int target_state, const int node)
{
    uint32_t ts1_count[MAX_LINK_WIDTH] = {0}, ts2_count[MAX_LINK_WIDTH] = {0};   // openPCIE: read before the first ReadEventCount()
    int i, lnkwidth;
    TS_t ts_status;
    const bool rc = (ltssm_profile[node] == LTSSM_PROFILE_ARTIX7_RC);
    // openPCIE: the substates' timeouts, in TS times (4.2.6.3): 24 ms in
    // Linkwidth.Start (Linkwidth.Accept is folded into its loop, so its 2 ms
    // is part of Start's 24 ms; the A7RC Accept loop of 10 TS1 has none), 2 ms
    // in Lanenum.Wait and Complete, then Detect; 2 ms in Idle, then Recovery
    // once, then Detect. Lanenum.Accept has none in the spec; our 2 ms there
    // is a guard against a partner gone quiet. Upstream had none: a port whose
    // partner gave up stayed in Configuration for ever (issue 63)
    const int cfg_ts_ms = 1000 * SYMCLK_TICKS_PER_US(ltssm_cur_gen[node]) / TS_TICKS;
    int       cfg_ts;

    lnkwidth = (active_lanes & 0x8000) ? 16 :
               (active_lanes & 0x80)   ? 8  :
               (active_lanes & 0x8)    ? 4  :
               (active_lanes & 0x2)    ? 2  :
                                         1;

    // Linkwidth.Start
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Start (node %d)\n", node);
    ltssm_idle_ns[node] = 0;

    // If not done so before, randomly choose to go to disabled state
    if (config_disable[node] == false && ((ltssm_force_tests[node] & ENABLE_DISABLE) || ((ltssm_enable_tests[node] & ENABLE_DISABLE) && ((PcieRand(node) % 3) == 0))))
    {
        config_disable[node] = true;
        if (!ltssm_disable_disp_state[node]) VPrint("---> Going to Disabled from Configuration Start (node %d)\n", node);
        return LTSSM_DISABLED;
    }

    // If not done so before, randomly choose to go to loopback state
    if (config_loopback[node] == false && ((ltssm_force_tests[node] & ENABLE_LOOPBACK) || ((ltssm_enable_tests[node] & ENABLE_LOOPBACK) && ((PcieRand(node) % 3) == 0))))
    {
        config_loopback[node] = true;
        if (!ltssm_disable_disp_state[node]) VPrint("---> Going to Loopback from Configuration Start (node %d)\n", node);
        return LTSSM_LOOPBACK;
    }

    ResetEventCount(TS1_ID, node);

    // The Artix-7 RC, a downstream port, opens with one more TS2, then assigns
    // the Link number at once
    if (rc)
    {
        SendTs(TS2_ID, PAD, PAD, ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
    }

    // As above, an upstream port must not put a Link number on the wire before
    // the downstream port has assigned one -- it echoes what it receives. Going
    // straight to the configured link number races the downstream port, which
    // is still in Configuration.Linkwidth.Start expecting Link = PAD.
    int linknum_rcvd = rc;

    cfg_ts = 0;
    do
    {
        SendTs(TS1_ID, PAD, linknum_rcvd ? ltssm_linknum[node] : PAD, ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
        ReadEventCount(TS1_ID, ts1_count, node);
        ts_status = GetTS(0, node);

        if (ts1_count[0] && ts_status.linknum == PAD)
        {
            ts1_count[0] = 0;
            ResetEventCount(TS1_ID, node);
        }
        else if (ts1_count[0])
        {
            linknum_rcvd = 1;
        }

        if (CfgTimedOut(&cfg_ts, 24 * cfg_ts_ms, "Linkwidth Start", node)) return LTSSM_DETECT;
    } while(ts1_count[0] < 2 || ts_status.linknum != ltssm_linknum[node]);

    // Linkwidth.Accept (fall through state, but for the Artix-7 RC)
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Linkwidth Accept (node %d)\n", node);
    for (i = 0; rc && i < A7RC_LINKWIDTH_ACCEPT_TS1; i++)
    {
        SendTs(TS1_ID, PAD, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
    }

    // Lanenum.Wait
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Lanenum Wait (node %d)\n", node);
    ResetEventCount(TS1_ID, node);

    // An upstream port must keep transmitting Lane = PAD until the downstream
    // port has assigned lane numbers -- it is the downstream port that does the
    // assigning (PCIe base spec, Configuration.Lanenum.Wait). Sending lane
    // numbers straight away works when the link partner is another copy of this
    // model, but deadlocks against a real downstream port: that port is still
    // waiting for 2 consecutive TS1 with Link = N and Lane = PAD to confirm the
    // link width, and a lane number in that field resets its count.
    // The Artix-7 RC assigns them after one more TS1 with Lane = PAD.
    int lanenums_rcvd = 0;
    int lanenum_sent  = 0;
    int reversed_seen = 0;   // TS1 times in a row with the lane numbers reversed
    cfg_ts = 0;

    // openPCIE: lane reversal (4.2.4.9.1, 4.2.6.3.2-3). Lane numbers received
    // strictly reversed (lane 0 gets n-1, lane n-1 gets 0) on two TS1s: a port
    // that may reverse its lanes does so (SetLaneReversal()); an upstream port
    // that may not sends its own numbers, which differ, and the downstream port
    // fixes the order or the link does not form. Upstream waited for its own
    // numbers on every lane, for ever on a reversed link
    do
    {
        int in_order = 1;
        int reversed = (lnkwidth > 1);

        SendTs(TS1_ID, (lanenums_rcvd || (rc && lanenum_sent++)) ? ENABLE_LANENUMS : PAD, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
        for (i=0; i < lnkwidth; i++)
        {
            ts_status = GetTS(i, node);
            ReadEventCount(TS1_ID, ts1_count, node);
            if (ts_status.linknum != ltssm_linknum[node] || ts_status.lanenum != i)
            {
                in_order = 0;
            }
            else
            {
                lanenums_rcvd = 1;
            }
            if (ts_status.linknum != ltssm_linknum[node] || ts_status.lanenum != lnkwidth-1-i)
            {
                reversed = 0;
            }
        }

        reversed_seen = reversed ? reversed_seen + 1 : 0;
        if (reversed_seen >= 2)
        {
            reversed_seen = 0;
            if (ltssm_lane_rev[node])
            {
                SetLaneReverse(!GetLaneReverse(node), node);
                if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Lanenum Wait: lanes %s (node %d)\n",
                                                            GetLaneReverse(node) ? "reversed" : "in order", node);
            }
            else
            {
                lanenums_rcvd = 1;
            }
        }

        if (!in_order)
        {
            ts1_count[0] = 0;
            ResetEventCount(TS1_ID, node);
        }
        if (CfgTimedOut(&cfg_ts, 2 * cfg_ts_ms, "Lanenum Wait", node)) return LTSSM_DETECT;
    } while (ts1_count[0] < 2);

    // Lanenum.Accept
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Lanenum Accept (node %d)\n", node);
    ResetEventCount(TS1_ID, node);

    // Hold this state for a while after the receive condition is met. The
    // partner has to see these TS1s too, and a real downstream port needs a
    // run of them in Configuration.Lanenum.Accept before it will move on. The
    // unguarded loop can exit after as few as three transmitted TS1, which is
    // not enough, and the link then times out back to Detect.
    int ts1_sendcount = 0;
    int ts1_more      = Vary(node);
    cfg_ts = 0;

    do
    {
        SendTs(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);

        if (ts1_count[0])
        {
            ts1_sendcount++;
        }

        // openPCIE: Link and Lane numbers must match on every lane (||);
        // upstream's && took a lane with either right (issue 61)
        for (i=0; i < lnkwidth; i++)
        {
            ts_status = GetTS(i, node);
            ReadEventCount(TS1_ID, ts1_count, node);
            if (ts_status.linknum != ltssm_linknum[node] || ts_status.lanenum != i)
            {
                ts1_count[0] = 0;
                ResetEventCount(TS1_ID, node);
            }
        }
        if (CfgTimedOut(&cfg_ts, 2 * cfg_ts_ms, "Lanenum Accept", node)) return LTSSM_DETECT;
    } while (ts1_count[0] < 2 || ts1_sendcount < (rc ? A7RC_LANENUM_ACCEPT_TS1 : 16) + ts1_more);

    // Complete
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Complete (node %d)\n", node);
    ResetEventCount(TS2_ID, node);
    int ts2_sendcount = 0;
    int ts2_more      = Vary(node);
    cfg_ts = 0;

    // The Artix-7 RC: one more TS1, then TS2 with the Autonomous Change bit
    if (rc)
    {
        SendTs(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node], gen & 0x4, node);
    }

    do
    {
        SendTsGen(TS2_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node],
                  gen | (rc ? TS_DATA_RATE_CHANGE_AUTO : 0), node);

        // Start counting sent TS2s once a TS2 has been received
        if (ts2_count[0])
        {
            ts2_sendcount++;
        }
        // openPCIE: as in Lanenum.Accept, every lane must match (||), and a
        // mismatch restarts the count; upstream's zero was overwritten by the
        // next lane's read (issue 61)
        int mismatch = 0;
        for (i=0; i < lnkwidth; i++)
        {
            ts_status = GetTS(i, node);
            ReadEventCount(TS2_ID, ts2_count, node);
            if (ts_status.linknum != ltssm_linknum[node] || ts_status.lanenum != i)
            {
                mismatch = 1;
            }
        }
        if (mismatch)
        {
            ts2_count[0] = 0;
            ResetEventCount(TS2_ID, node);
        }
        if (CfgTimedOut(&cfg_ts, 2 * cfg_ts_ms, "Complete", node)) return LTSSM_DETECT;
    } while((ts2_count[0] < 8) || ts2_sendcount < (rc ? A7RC_COMPLETE_TS2 : 16) + ts2_more);

    ltssm_tx_n_fts[node] = ts_status.n_fts;

    // Idle
    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration Idle (node %d)\n", node);
    ltssm_idle_ns[node] = GetLinkTimeNs(node);
    i = 0;
    int idle_n = 16 + Vary(node);   // idle symbols sent from the first received (4.2.6.3.6)
    cfg_ts = 0;
    ResetEventCount(0, node);
    do
    {
        SendIdle(1, node);
        ReadEventCount(0, ts2_count, node);

        if (ts2_count[0])
        {
            i++;
        }
        DebugVPrint("--->i = %d ts2_count[0] = %d (node %d)\n", i, ts2_count[0], node);
        if (CfgTimedOut(&cfg_ts, 2 * cfg_ts_ms * TS_TICKS, "Idle", node))
        {
            ltssm_idle_ns[node] = 0;
            if (!ltssm_idle_to_rlock[node])   // first: Recovery, then Detect (4.2.6.3.6)
            {
                ltssm_idle_to_rlock[node] = true;
                return LTSSM_RECOVERY;
            }
            return LTSSM_DETECT;
        }
    } while(i < idle_n || ts2_count[0] < 8);

    if (!ltssm_disable_disp_state[node]) VPrint("---> Configuration exit to L0 (node %d)\n", node);
    ltssm_idle_to_rlock[node] = false;

    // L0: clear the TS counts LinkIdle() watches
    ResetEventCount(TS1_ID, node);
    ResetEventCount(TS2_ID, node);

    // Exit to L0
    return LTSSM_L0;
}

// -------------------------------------------------------------------------
// Vary()
//
// openPCIE: 0 to ltssm_vary[node] more, at random (PcieRand()), over a count
// the spec only bounds from below: TS1/TS2 in Configuration and Recovery,
// idle symbols, EIOS, the idle in Recovery.Speed
//
// -------------------------------------------------------------------------

static int Vary(const int node)
{
    return ltssm_vary[node] ? (int)(PcieRand(node) % (uint32_t)(ltssm_vary[node] + 1)) : 0;
}

// -------------------------------------------------------------------------
// SendEios()
//
// openPCIE: EIOS before electrical idle, one at 2.5 GT/s and two at 5.0 GT/s
// (4.2.4.2). Upstream sent one at any rate
//
// -------------------------------------------------------------------------

static void SendEios(const int node)
{
    for (int i = 0; i < ltssm_cur_gen[node]; i++)
    {
        SendOs(IDL, node);
    }
}

// -------------------------------------------------------------------------
// TxL0s()
// -------------------------------------------------------------------------

static int TxL0s (const int target_state, const int active_lanes, const int ticks, const int node)
{
    int i;

    // ---------------
    if (!ltssm_disable_disp_state[node])  VPrint("---> TxL0s Entry (node %d)\n", node);

    // Inform model that the transmitter is down (and thus queue their data)
    SetTxDisabled(node);

    SendEios(node);
    // Shut down the lanes
    VWrite(LINK_STATE, 0xffff, 1, node);

    // ---------------
    if (!ltssm_disable_disp_state[node]) VPrint("---> TxL0s Idle: sleeping for %d ticks (node %d)\n", ticks, node);
    SendIdle(ticks, node);

    // ---------------
    if (!ltssm_disable_disp_state[node]) VPrint("---> TxL0s FTS (node %d)\n", node);
    VWrite(LINK_STATE, ~(active_lanes & ltssm_max_link_mask[node]) & 0xffff, 1, node);
    for (i = 0; i < ltssm_tx_n_fts[node]; i++)
    {
        SendOs(FTS, node);
    }

    SendOs(SKP, node);

    // The transmitter is available once again
    SetTxEnabled(node);

    return LTSSM_L0;
}

// -------------------------------------------------------------------------
// RecoveryStuck()
//
// openPCIE: one line when a Recovery has run RECOVERY_STUCK_NS, in the
// substate it is stuck in
//
// -------------------------------------------------------------------------

static void RecoveryStuck(const uint64_t entry_ns, bool *told, const char *substate, const int node)
{
    if (!*told && GetLinkTimeNs(node) - entry_ns > RECOVERY_STUCK_NS)
    {
        VPrint("---> Recovery for %d us, still in %s at Gen%d: does the partner's TS1/TS2 get through? (node %d)\n",
               RECOVERY_STUCK_NS / 1000, substate, ltssm_cur_gen[node], node);
        *told = true;
    }
}

// -------------------------------------------------------------------------
// Recovery()
// -------------------------------------------------------------------------

static int Recovery (const int gen, const int target_state, const int node)
{
    uint32_t ts1_count[MAX_LINK_WIDTH], ts2_count[MAX_LINK_WIDTH], idl_count[MAX_LINK_WIDTH];
    TS_t ts_status;
    int  i, rx_count, tx_count, timeout, rate, ticks_us;
    int  state       = RECOVERY_RCVRLOCK;
    int  entry_gen   = ltssm_cur_gen[node];
    int  new_gen     = entry_gen;
    int  idle_us     = RECOVERY_SPEED_IDLE_US;
    int  partner     = 0;

    (void)gen;              // openPCIE: the rates come from ltssm_rate[]
    (void)target_state;

    // directed_speed_change: set by our own request or by the partner's speed_change bit.
    // An own request is autonomous: TS2 carry the Autonomous Change bit.
    bool directed    = ltssm_speed_req[node];
    bool autonomous  = ltssm_speed_req[node];

    // changed_speed_recovery: the rate has changed since this Recovery started
    bool changed     = false;

    const bool rc    = (ltssm_profile[node] == LTSSM_PROFILE_ARTIX7_RC);
    int  sc_count, idle_ns;
    bool sc, locked, to_speed;
    int  more = 0;
    uint64_t entry_ns = GetLinkTimeNs(node);
    bool     told     = false;

    ltssm_speed_req[node]   = false;
    ltssm_retrain_req[node] = false;

    // Hold TLPs and DLLPs until the link is back in L0
    SetTxDisabled(node);

    while (state != RECOVERY_IDLE)
    {
        switch (state)
        {
        // --- RcvrLock ---
        // Send TS1 and look for 8 TS1 or TS2 whose speed_change bit matches ours.
        // A speed_change bit from the partner makes it our speed change too.
        case RECOVERY_RCVRLOCK:
            if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Lock at Gen%d%s (node %d)\n", ltssm_cur_gen[node], directed ? ", speed change" : "", node);
            ltssm_idle_ns[node] = 0;
            ResetEventCount(TS1_ID, node);
            ResetEventCount(TS2_ID, node);
            rx_count = 0;
            tx_count = 0;
            sc_count = 0;
            locked   = false;
            timeout  = RECOVERY_RCVRLOCK_TO_US * SYMCLK_TICKS_PER_US(ltssm_cur_gen[node]) / TS_TICKS;

            for (i = 0; !locked && i < timeout; i++)
            {
                RecoveryStuck(entry_ns, &told, "RcvrLock", node);
                // The Artix-7 RC holds the speed_change bit for its first TS1 at the old rate
                sc = directed && !(rc && !changed && tx_count < (autonomous ? A7RC_LOCK_HOLD_OWN : A7RC_LOCK_HOLD_PARTNER));
                SendTsGen(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node],
                          ltssm_rate[node] | (sc ? TS_DATA_RATE_CHANGE_SPEED : 0), node);
                tx_count++;
                sc_count += sc;
                ReadEventCount(TS1_ID, ts1_count, node);
                ReadEventCount(TS2_ID, ts2_count, node);

                if (ts1_count[0] || ts2_count[0])
                {
                    ts_status = GetTS(0, node);
                    directed |= (ts_status.datarate & TS_DATA_RATE_CHANGE_SPEED) != 0;
                    rx_count  = (((ts_status.datarate & TS_DATA_RATE_CHANGE_SPEED) != 0) == directed) ? rx_count + ts1_count[0] + ts2_count[0] : 0;
                    ResetEventCount(TS1_ID, node);
                    ResetEventCount(TS2_ID, node);
                }

                // 8 received; the Artix-7 RC also sends its minimum of TS1
                locked = (rx_count >= 8) &&
                         (!rc || (changed ? tx_count >= (ltssm_cur_gen[node] == 2 ? A7RC_LOCK_NEW_GEN2 : A7RC_LOCK_NEW_GEN1)
                                          : !directed || sc_count >= (autonomous ? A7RC_LOCK_SC_OWN : A7RC_LOCK_SC_PARTNER)));
            }

            if (locked)
            {
                state = RECOVERY_RCVRCFG;
            }
            // Timed out: go back to the rate Recovery started at, or down to Gen1
            else if (changed || ltssm_cur_gen[node] > 1)
            {
                new_gen  = changed ? entry_gen : 1;
                idle_us  = RECOVERY_SPEED_FAIL_IDLE_US;
                changed  = false;
                state    = RECOVERY_SPEED;
            }
            else
            {
                if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Lock timed out: to Detect (node %d)\n", node);
                SetTxEnabled(node);
                return LTSSM_DETECT;
            }
            break;

        // --- RcvrCfg ---
        // Send TS2 and look for 8 TS2 whose speed_change bit matches ours. Without
        // a speed change, exit to Idle after 16 TS2 sent from the first one received.
        // With one, exit to Speed after 32 (4.2.6.4.3; the Artix-7 RC profile also
        // when the partner sends EIOS first, maybe before 32: a deviation, kept as
        // its speed change timing is calibrated with it); but at 2.5 GT/s only when both sides advertise 5.0 GT/s,
        // else to Idle as without. Upstream went to Speed, and sent EIOS
        case RECOVERY_RCVRCFG:
            if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery RcvrCfg (node %d)\n", node);
            ResetEventCount(TS2_ID, node);
            ResetEventCount(IDL, node);
            rx_count = 0;
            tx_count = 0;
            to_speed = false;
            more     = Vary(node);
            timeout  = RECOVERY_RCVRCFG_TO_US * SYMCLK_TICKS_PER_US(ltssm_cur_gen[node]) / TS_TICKS;

            for (i = 0; i < timeout; i++)
            {
                RecoveryStuck(entry_ns, &told, "RcvrCfg", node);
                rate = ltssm_rate[node] | (directed ? TS_DATA_RATE_CHANGE_SPEED : 0) | (directed && autonomous ? TS_DATA_RATE_CHANGE_AUTO : 0);
                SendTsGen(TS2_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], ltssm_ts_ctl[node], rate, node);
                ReadEventCount(TS2_ID, ts2_count, node);
                ReadEventCount(IDL, idl_count, node);

                if (ts2_count[0])
                {
                    ts_status = GetTS(0, node);
                    partner   = ts_status.datarate;
                    directed |= (ts_status.datarate & TS_DATA_RATE_CHANGE_SPEED) != 0;
                    rx_count  = (((ts_status.datarate & TS_DATA_RATE_CHANGE_SPEED) != 0) == directed) ? rx_count + ts2_count[0] : 0;
                    ResetEventCount(TS2_ID, node);
                }

                if (rx_count)
                {
                    tx_count++;
                }

                to_speed = directed && (ltssm_cur_gen[node] > 1 || (ltssm_rate[node] & partner & TS_DATA_RATE_5G));
                if (rx_count >= 8 && (tx_count >= (to_speed ? SPEED_CHANGE_TS2_COUNT : rc ? A7RC_CFG_IDLE_TS2 : 16) + more ||
                                      (rc && to_speed && idl_count[0])))
                {
                    break;
                }
            }

            if (i == timeout)
            {
                if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery RcvrCfg timed out: to Detect (node %d)\n", node);
                SetTxEnabled(node);
                return LTSSM_DETECT;
            }

            ltssm_tx_n_fts[node] = ts_status.n_fts;

            if (to_speed)
            {
                // The highest rate both sides advertise
                new_gen = (ltssm_rate[node] & partner & TS_DATA_RATE_5G) ? 2 : 1;
                idle_us = RECOVERY_SPEED_IDLE_US;
                changed = (new_gen != entry_gen);
                state   = RECOVERY_SPEED;
            }
            else
            {
                state   = RECOVERY_IDLE;
            }
            break;

        // --- Speed ---
        // Send EIOS (two at 5 GT/s) and go electrically idle. Once the partner is
        // idle too, stay idle for the minimum time at the new rate, then back to
        // RcvrLock. The rate itself is the PHY's: it follows the partner's PIPE rate.
        case RECOVERY_SPEED:
            if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Speed: Gen%d -> Gen%d (node %d)\n", ltssm_cur_gen[node], new_gen, node);
            SendEios(node);                     // the least, as the hard macro
            for (i = 0, more = Vary(node) % 3; i < more; i++)
            {
                SendOs(IDL, node);
            }
            ltssm_eios_ns[node] = GetLinkTimeNs(node);
            VWrite(LINK_STATE, 0xffff, 1, node);

            // The symbol clock follows the RC's rate, which may change while we
            // wait: time both waits at the faster rate, so they last at least
            // as long as asked
            ticks_us = SYMCLK_TICKS_PER_US((new_gen > ltssm_cur_gen[node]) ? new_gen : ltssm_cur_gen[node]);

            for (i = 0; i < RECOVERY_SPEED_FAIL_IDLE_US * ticks_us; i++)
            {
                ReadEventCount(IDL, idl_count, node);
                if (idl_count[0])
                {
                    break;
                }
                SendIdle(1, node);
            }

            // The Artix-7 RC profile is the root port: it changes the rate, with
            // both sides idle. GEN2_CLK drives Gen2ClkSel, the test bench's clocks.
            // GetLinkSpeed() follows at once: the UpdateFC timer counts time with it.
            // First let our EIOS reach the partner: when its EIOS came first, a
            // rate change at once garbles ours, and the partner waits out its
            // idle timeout. Its idle is a time from its last EIOS (A7RC_SPEED_IDLE_NS_*):
            // so far that EIOS, the wait above and A7RC_SPEED_EIOS_TICKS, at the old rate
            if (rc)
            {
                SendIdle(A7RC_SPEED_EIOS_TICKS, node);
                idle_ns = (4 + i + A7RC_SPEED_EIOS_TICKS) * 4 / ltssm_cur_gen[node];
                LinkTimeUpdate(node);
                VWrite(GEN2_CLK, new_gen == 2, 1, node);
                ltssm_cur_gen[node] = new_gen;

                idle_ns = (idle_us == RECOVERY_SPEED_IDLE_US)
                        ? ((new_gen == 2) ? A7RC_SPEED_IDLE_NS_GEN2 : A7RC_SPEED_IDLE_NS_GEN1) + 16 * Vary(node) - idle_ns
                        : idle_us * 1000;
                if (idle_ns * new_gen / 4 > 0)
                {
                    SendIdle(idle_ns * new_gen / 4, node);
                }
            }
            else
            {
                // On the serial line our own wrapper's clocks follow GEN2_CLK
                // (PcieVhostSerial, Gen2Clk = 1); on a PIPE the test bench's do,
                // and Gen2ClkSel is left open. A delta-time write: no tick.
                // First our last EIOS leaves the serialiser: a rate change
                // under it sent its last symbol half at each rate
                SendIdle(SPEED_EIOS_DRAIN_TICKS, node);
                LinkTimeUpdate(node);
                VWrite(GEN2_CLK, new_gen == 2, 1, node);
                ltssm_cur_gen[node] = new_gen;
                SendIdle(idle_us * ticks_us, node);
            }

            LinkTimeUpdate(node);
            directed            = false;
            autonomous          = false;
            ResetEventCount(IDL, node);
            VWrite(LINK_STATE, ~ltssm_max_link_mask[node] & 0xffff, 1, node);
            state = RECOVERY_RCVRLOCK;
            break;
        }
    }

    // --- Idle ---
    if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Idle (node %d)\n", node);
    ltssm_idle_ns[node] = GetLinkTimeNs(node);
    // Exit after 8 idle symbols received and 16 sent from the first one received
    i       = 0;
    more    = 16 + Vary(node);
    timeout = RECOVERY_IDLE_TO_US * SYMCLK_TICKS_PER_US(ltssm_cur_gen[node]);
    ResetEventCount(0, node);
    do
    {
        if (timeout-- == 0)
        {
            // openPCIE: first to Recovery.RcvrLock, then Detect (4.2.6.4.4);
            // upstream went to Detect at once
            ltssm_idle_ns[node] = 0;
            SetTxEnabled(node);
            if (!ltssm_idle_to_rlock[node])
            {
                if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Idle timed out: to Recovery (node %d)\n", node);
                ltssm_idle_to_rlock[node] = true;
                return LTSSM_RECOVERY;
            }
            if (!ltssm_disable_disp_state[node]) VPrint("---> Recovery Idle timed out: to Detect (node %d)\n", node);
            return LTSSM_DETECT;
        }

        SendIdle(1, node);
        ReadEventCount(0, ts2_count, node);

        if (ts2_count[0])
        {
            i++;
        }
    } while(i < more || ts2_count[0] < 8);

    if (!ltssm_disable_disp_state[node]) VPrint("---> Leaving Recovery at Gen%d (node %d)\n", ltssm_cur_gen[node], node);
    ltssm_idle_to_rlock[node] = false;

    // L0: clear the TS counts LinkIdle() watches, and release TLPs and DLLPs
    ResetEventCount(TS1_ID, node);
    ResetEventCount(TS2_ID, node);
    SetTxEnabled(node);

    // Exit to L0
    return LTSSM_L0;
}

// -------------------------------------------------------------------------
// Disabled()
// -------------------------------------------------------------------------

static int Disabled (const int gen, const int node)
{
    int i, rand_idle;
    uint32_t idl_count[MAX_LINK_WIDTH];

    if (!ltssm_disable_disp_state[node]) VPrint("---> Disabled (node %d)\n", node);

    ResetEventCount(IDL, node);
    // Transmit 16 TS1 OS's with disabled set
    for (i=0; i < 16; i++)
    {
       SendTs(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], TS_CNTL_DISABLE_LINK, gen & 0x4, node);
    }

    // Tx EIOS
    SendEios(node);

    // Shut down the lanes
    VWrite(LINK_STATE, 0xffff, 1, node);

    // Wait for an EIOS from unit
    do
    {
        ReadEventCount(IDL, idl_count, node);
        DebugVPrint("---> idl_count[0] = %d (node %d)\n", idl_count[0], node);
        SendIdle(1, node);
    } while (!idl_count[0]);

    //Send a random amount of idles (i.e. wait for a set time---this link is electrically idle)
    //rand_idle = (PcieRand(node) % 1000) + 25;
    rand_idle = 100;

    if (!ltssm_disable_disp_state[node]) VPrint("---> Waiting for %d ticks (node %d)\n", rand_idle, node);
    SendIdle(rand_idle, node);

    if (!ltssm_disable_disp_state[node]) VPrint("---> Leaving Disabled for Detect (node %d)\n", node);

    return LTSSM_DETECT;
}

// -------------------------------------------------------------------------
// Loopback()
// -------------------------------------------------------------------------

static int Loopback (const int gen, const int node)
{
    int i, rand_idle;
    TS_t ts_status;
    uint32_t count[MAX_LINK_WIDTH];

    if (!ltssm_disable_disp_state[node]) VPrint("---> Loopback (node %d)\n", node);

    // ---- Loopback.Entry ----

    ResetEventCount(TS1_ID, node);

    // Transmit TS1s OS's with loopback set until a TS1 with loopback set is received
    do
    {
        SendTs(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], TS_CTL_LOOPBACK, gen & 0x4, node);
        ReadEventCount(TS1_ID, count, node);
        ts_status = GetTS(0, node);
        if (!ltssm_disable_disp_state[node]) VPrint("count[0] = %x ts_status.control = %x\n", count[0], ts_status.control);
    } while (count[0] == 0 || !(ts_status.control & TS_CNTL_LOOPBACK));

    // ---- Loopback.Active ----
    if (!ltssm_disable_disp_state[node]) VPrint("---> Loopback.Active (node %d)\n", node);

    // Stay in Loopback.Active for a while
    for (i = 0; i < 64; i++)
    {
        SendTs(TS1_ID, ENABLE_LANENUMS, ltssm_linknum[node], ltssm_n_fts[node], TS_CTL_LOOPBACK, gen & 0x4, node);
    }

    // ---- Loopback.Exit ----

    if (!ltssm_disable_disp_state[node]) VPrint("---> Loopback.Exit (node %d)\n", node);
    ResetEventCount(IDL, node);

    // Send and Electrical Idle
    SendEios(node);

    // Shut down the lanes
    VWrite(LINK_STATE, 0xffff, 1, node);

    // Wait for the idle to be returned
    do
    {
        ReadEventCount(IDL, count, node);
        DebugVPrint("---> count[0] = %d (node %d)\n", count[0], node);
        SendIdle(1, node);
    } while (!count[0]);

    //Send a random amount of idles (i.e. wait for a set time---this link is electrically idle)
    //rand_idle = (PcieRand(node) % 1000) + 25;
    rand_idle = 1000;

    if (!ltssm_disable_disp_state[node]) VPrint("---> Waiting for %d ticks (node %d)\n", rand_idle, node);
    SendIdle(rand_idle, node);

    if (!ltssm_disable_disp_state[node]) VPrint("---> Leaving Loopback for Detect (node %d)\n", node);

    return LTSSM_DETECT;
}

// -------------------------------------------------------------------------
// HotReset()
// -------------------------------------------------------------------------

static int HotReset (const int HotResetTO, const int gen, const int node)
{
    int loops = HotResetTO;
    int i;

    if (loops < 2)
    {
       loops = 2;
    }

    for (i=0; i < loops; i++)
    {
        SendTs(TS1_ID, 0, ltssm_linknum[node], ltssm_n_fts[node], TS_CNTL_HOT_RESET, gen & 0x4, node);
    }

    return LTSSM_DETECT;
}

// -------------------------------------------------------------------------
// L1()
// -------------------------------------------------------------------------

static int L1 (const int time_in_l1_param, const int node)
{
    int time_in_l1 = time_in_l1_param;

    if (time_in_l1 < 30)
    {
        time_in_l1 = 30;
    }

    // Shut down the lanes
    VWrite(LINK_STATE, 0xffff, 1, node);

    // Stay in L1 state for a minimum amount of time
    SendIdle(time_in_l1, node);

    return LTSSM_RECOVERY;
}

// -------------------------------------------------------------------------
// L2()
// -------------------------------------------------------------------------

static int L2 (const int time_in_l2_param, const int node)
{
    int time_in_l2 = time_in_l2_param;

    if (time_in_l2 < 10)
    {
        time_in_l2 = 10;
    }

    // Shut down the lanes
    VWrite(LINK_STATE, 0xffff, 1, node);

    // Stay in L2 state for a minimum amount of time
    SendIdle(time_in_l2, node);

    return LTSSM_DETECT;
}

// -------------------------------------------------------------------------
// LinkState()
//
// Top level LTSSM state machine function
//
// -------------------------------------------------------------------------

static int LinkState (const int ltssm_state, const int target_state, const int link_width, const int gen, const int node)
{
    // openPCIE: the lanes Polling found active, kept per node for
    // Configuration, a later call. Upstream's local was 0 again by then, so
    // Configuration checked lane 0 only, at any width (issue 59)
    static int active_lanes[VP_MAX_NODES];

    ltssm_cur[node] = ltssm_state;              // openPCIE: the state now in, for GetLtssmState()

    switch (ltssm_state) {
    case LTSSM_DETECT:
        return Detect(link_width, node);
        break;

    case LTSSM_POLLING:
        return Polling(&active_lanes[node], gen, node);
        break;

    case LTSSM_CONFIG:
        return Configuration(active_lanes[node], gen, target_state, node);
        break;

    case LTSSM_DISABLED:
        return Disabled(gen, node);
        break;

    case LTSSM_LOOPBACK:
        return Loopback(gen, node);
        break;

    case LTSSM_HOTRESET:
        return HotReset(LTSSM_SET_MINIMUM, gen, node);
        break;

    case LTSSM_L1:
        return L1(LTSSM_SET_MINIMUM, node);
        break;

    case LTSSM_L2:
        return L2(LTSSM_SET_MINIMUM, node);
        break;

    case LTSSM_RECOVERY:
        return Recovery(gen, target_state, node);
        break;

    case LTSSM_L0s:
    default:
        VPrint ("LinkState(): ***Error: InitLink() reached unsupported or invalid LTSSM state (%d)\n", ltssm_state);
        VWrite(PVH_FATAL, 0, 0, node);
        break;
    }

    return ltssm_state;
}

// -------------------------------------------------------------------------
// InitLink()/InitLinkGen()
//
// Exported user function to initiate a PCIe link initialisation.
//
// -------------------------------------------------------------------------

void InitLinkGen(const int link_width, const int gen, const int node)
{
    int ltssm_state = LTSSM_DETECT;

    ltssm_rate[node] = gen;

    do
    {
        ltssm_state = LinkState(ltssm_state, LTSSM_L0, link_width, gen, node);
    } while (ltssm_state != LTSSM_L0);
    ltssm_cur[node]   = LTSSM_L0;
    ltssm_l0_ns[node] = GetLinkTimeNs(node);
}

void InitLink(const int link_width, const int node)
{
    InitLinkGen(link_width, TS_DATA_RATE_GEN1, node);
}

// -------------------------------------------------------------------------
// LinkIdle()
//
// Exported user function (openPCIE): send idles in L0 for 'ticks', and follow
// the partner into Recovery when its TS1 or TS2 arrive, or run a speed change
// that ChangeLinkSpeed() asked for. Returns in L0.
//
// -------------------------------------------------------------------------

void LinkIdle(const int ticks, const int node)
{
    uint32_t ts1_count[MAX_LINK_WIDTH] = {0}, ts2_count[MAX_LINK_WIDTH] = {0};   // openPCIE: read before the first ReadEventCount()
    int ltssm_state, n, left = ticks;

    while (left > 0)
    {
        // Look before sending, so that nothing more goes out once the partner is in Recovery
        if (!ltssm_speed_req[node] && !ltssm_retrain_req[node])
        {
            ReadEventCount(TS1_ID, ts1_count, node);
            ReadEventCount(TS2_ID, ts2_count, node);
            if (!ts1_count[0] && !ts2_count[0])
            {
                n = (left < L0_POLL_TICKS) ? left : L0_POLL_TICKS;
                SendIdle(n, node);
                left -= n;
                continue;
            }
        }

        uint64_t recovery_start = GetLinkTimeNs(node);
        uint32_t recovery_tick  = GetCycleCount(node);
        ltssm_recovery_from[node]  = recovery_start;
        ltssm_recovery_tfrom[node] = recovery_tick;
        ltssm_in_recovery[node]   = true;

        ltssm_state = LTSSM_RECOVERY;
        do
        {
            ltssm_state = LinkState(ltssm_state, LTSSM_L0, ltssm_max_link_width[node], ltssm_rate[node], node);
        } while (ltssm_state != LTSSM_L0);
        ltssm_cur[node]   = LTSSM_L0;
        ltssm_l0_ns[node] = GetLinkTimeNs(node);

        ltssm_recovery_ns[node]    += GetLinkTimeNs(node) - recovery_start;
        ltssm_recovery_ticks[node] += GetCycleCount(node) - recovery_tick;
        ltssm_in_recovery[node]     = false;
    }
}

// -------------------------------------------------------------------------
// ChangeLinkSpeed()
//
// Exported user function (openPCIE): change the link to Gen1 or Gen2. We then
// advertise only the rates up to 'gen', and the next LinkIdle() takes the link
// through Recovery with the speed_change bit set.
//
// -------------------------------------------------------------------------

void ChangeLinkSpeed(const int gen, const int node)
{
    ltssm_rate[node]      = (gen == 2) ? TS_DATA_RATE_GEN2 : TS_DATA_RATE_GEN1;
    ltssm_speed_req[node] = true;
}

// -------------------------------------------------------------------------
// RetrainLink()
//
// Exported user function (openPCIE): the next LinkIdle() takes the link
// through Recovery without a speed change (4.2.6.5: directed; the Retrain
// Link bit, 7.8.7)
//
// -------------------------------------------------------------------------

void RetrainLink(const int node)
{
    ltssm_retrain_req[node] = true;
}

// -------------------------------------------------------------------------
// GetLinkSpeed()
//
// Exported user function (openPCIE): the rate the link runs at, 1 or 2
//
// -------------------------------------------------------------------------

int GetLinkSpeed(const int node)
{
    return ltssm_cur_gen[node];
}

// -------------------------------------------------------------------------
// GetLinkTimeNs()
//
// Exported user function (openPCIE): ns since the model started, from the
// symbol clock ticks at each rate. Exact when the profile changes the rate
// (the Artix-7 RC); for a node that follows its partner's rate, it lags each
// change by Recovery.Speed
//
// -------------------------------------------------------------------------

uint64_t GetLinkTimeNs(const int node)
{
    LinkTimeUpdate(node);

    return ltssm_time_ns[node];
}

// -------------------------------------------------------------------------
// GetLinkRecoveryNs()
//
// Exported user function (openPCIE): of GetLinkTimeNs(), the ns the link
// spent in Recovery from L0, through LinkIdle(), with the one in progress, so
// that GetLinkTimeNs() less it stands still in Recovery and never steps back.
// Timers that run only in L0 (Ack latency, UpdateFC) leave it out
//
// -------------------------------------------------------------------------

uint64_t GetLinkRecoveryNs(const int node)
{
    return ltssm_recovery_ns[node] + (ltssm_in_recovery[node] ? GetLinkTimeNs(node) - ltssm_recovery_from[node] : 0);
}

// -------------------------------------------------------------------------
// GetLinkRecoveryTicks()
//
// Exported user function (openPCIE): the same in the model's ticks
// (GetCycleCount()), whatever the speed, for timers held in Recovery (the
// replay timer, 3.5.2.1)
//
// -------------------------------------------------------------------------

uint32_t GetLinkRecoveryTicks(const int node)
{
    return ltssm_recovery_ticks[node] + (ltssm_in_recovery[node] ? GetCycleCount(node) - ltssm_recovery_tfrom[node] : 0);
}

// -------------------------------------------------------------------------
// GetLtssmState() / GetLtssmL0Ns()
//
// Exported user functions (openPCIE): the LTSSM state the model is in
// (LTSSM_L0 between the calls that train the link), when it last entered
// L0 (GetLinkTimeNs()), the rates it advertises, and when it last sent EIOS in
// Recovery.Speed, for the Physical Layer checks
//
// -------------------------------------------------------------------------

int GetLtssmState(const int node)
{
    return ltssm_cur[node];
}

uint64_t GetLtssmL0Ns(const int node)
{
    return ltssm_l0_ns[node];
}

// SetLtssmVary(): exported (openPCIE): TS counts and idle times up to 'max'
// more than the least (Vary()); 0, the default: the least, or the profile's
void SetLtssmVary(const int max, const int node)
{
    ltssm_vary[node] = max;
}

// openPCIE: the port may reverse its lanes in Configuration (4.2.4.9.1)
void SetLaneReversal(const int on, const int node)
{
    ltssm_lane_rev[node] = on;
}

int GetLtssmRates(const int node)
{
    return ltssm_rate[node];
}

// openPCIE: when Configuration.Idle or Recovery.Idle began (GetLinkTimeNs()),
// 0 outside them: a partner may be in L0 once 8 idle symbols have gone out
uint64_t GetLtssmIdleNs(const int node)
{
    return (ltssm_cur[node] == LTSSM_L0) ? 0 : ltssm_idle_ns[node];
}

uint64_t GetLtssmEiosNs(const int node)
{
    return ltssm_eios_ns[node];
}

// -------------------------------------------------------------------------
// SetLinkProfile()
//
// Exported user function (openPCIE): the link partner this node plays,
// LTSSM_PROFILE_EP (the default) or LTSSM_PROFILE_ARTIX7_RC. Call it before
// InitLink().
//
// -------------------------------------------------------------------------

void SetLinkProfile(const int profile, const int node)
{
    ltssm_profile[node] = profile;
}

// -------------------------------------------------------------------------
// GetLinkProfile()
//
// Exported user function (openPCIE): the link partner this node plays. Flow
// control initialisation (pcie_utils.c) follows it too.
//
// -------------------------------------------------------------------------

int GetLinkProfile(const int node)
{
    return ltssm_profile[node];
}

// -------------------------------------------------------------------------
// ConfigLinkInit()
//
// Unless specified as a 'no change', update the configurations for this
// node with limiting masks to avoid bad values.
//
// -------------------------------------------------------------------------

void ConfigLinkInit (const ConfigLinkInit_t cfg, const int node)
{
    ltssm_linknum[node]            = (cfg.ltssm_linknum              == LINK_INIT_NO_CHANGE) ? ltssm_linknum[node]            : cfg.ltssm_linknum         & 0xff;
    ltssm_n_fts[node]              = (cfg.ltssm_n_fts                == LINK_INIT_NO_CHANGE) ? ltssm_n_fts[node]              : cfg.ltssm_n_fts           & 0xff;
    ltssm_ts_ctl[node]             = (cfg.ltssm_ts_ctl               == LINK_INIT_NO_CHANGE) ? ltssm_ts_ctl[node]             : cfg.ltssm_ts_ctl          & 0x1f;
    ltssm_detect_quiet_to[node]    = (cfg.ltssm_detect_quiet_to      == LINK_INIT_NO_CHANGE) ? ltssm_detect_quiet_to[node]    : cfg.ltssm_detect_quiet_to;
    ltssm_enable_tests[node]       = (cfg.ltssm_enable_tests         == LINK_INIT_NO_CHANGE) ? ltssm_enable_tests[node]       : cfg.ltssm_enable_tests;
    ltssm_force_tests[node]        = (cfg.ltssm_force_tests          == LINK_INIT_NO_CHANGE) ? ltssm_force_tests[node]        : cfg.ltssm_force_tests;
    ltssm_poll_tx_count[node]      = (cfg.ltssm_poll_active_tx_count == LINK_INIT_NO_CHANGE) ? ltssm_poll_tx_count[node]      : cfg.ltssm_poll_active_tx_count;
    ltssm_disable_disp_state[node] = (cfg.ltssm_disable_disp_state   == LINK_INIT_NO_CHANGE) ? ltssm_disable_disp_state[node] : cfg.ltssm_disable_disp_state;
}

// -------------------------------------------------------------------------
// ConfigurePcieLtssm()
//
// API function to configre the LTSSM. Uses config_t defined in pcie.h
//
// -------------------------------------------------------------------------

void ConfigurePcieLtssm(const config_t type, const int value, const int node)
{
    ConfigLinkInit_t ltssm_cfg;
    bool ltssm_cfg_updated = false;

    INIT_CFG_LINK_STRUCT(ltssm_cfg);

    switch (type)
    {
    case CONFIG_LTSSM_LINKNUM:
        ltssm_cfg.ltssm_linknum = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_N_FTS:
        ltssm_cfg.ltssm_n_fts = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_TS_CTL:
        ltssm_cfg.ltssm_ts_ctl = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_DETECT_QUIET_TO:
        ltssm_cfg.ltssm_detect_quiet_to = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_ENABLE_TESTS:
        ltssm_cfg.ltssm_enable_tests = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_FORCE_TESTS:
        ltssm_cfg.ltssm_force_tests = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_POLL_ACTIVE_TX_COUNT:
        ltssm_cfg.ltssm_poll_active_tx_count = value;
        ltssm_cfg_updated = true;
        break;

    case CONFIG_LTSSM_DISABLE_DISP_STATE:
        ltssm_cfg.ltssm_disable_disp_state = value;
        ltssm_cfg_updated = true;
        break;

    default:
        VPrint("ConfigurePcieLtssm: ***Error --- bad config type at node %d\n", node);
        VWrite(PVH_FATAL, 0, 0, node);
        break;
    }

    if (ltssm_cfg_updated)
    {
        ConfigLinkInit(ltssm_cfg, node);
    }
}
