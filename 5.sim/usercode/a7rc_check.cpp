// SPDX-FileCopyrightText: 2026 Chili.CHIPS
// SPDX-License-Identifier: BSD-3-Clause

//==========================================================================
// Copyright (c) 2026 Chili.CHIPS
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
//   The Artix-7 RC model's checker (a7rc_check.h). pcieVHost's packet
//   monitor hands it every TLP and DLLP: received from the endpoint before
//   the model processes them, and sent by the model. The rules below are
//   checked on the endpoint's side of the link, against the PCIe Base
//   Specification 2.1 (section numbers only). The line is error-free in
//   simulation, so a Nak or a replay is the endpoint's fault, not noise.
//
//   Rule IDs by layer: TL. Transaction Layer, DL. Data Link Layer, FC. flow
//   control (both layers), CFG. configuration space (judged from the
//   Configuration Requests the RC sends and their Completions), PL. Physical
//   Layer. The model checks its own packets too (self-check): a bad one is a
//   model error, not the endpoint's.
//==========================================================================

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <map>

#include "VProcClass.h"
#include "pcieModelClass.h"

extern "C"
{
#include "ltssm.h"

// From pcie_utils.c (pcie_utils.h is C only): write a packet's CRCs in place
void CalcLcrc   (PktData_t *pkt);
void CalcDllpCrc(PktData_t *dllp);
}

// pcie.h's TLP_HAS_DIGEST() and GET_TLP_FBE/LBE() use masks from pcie_utils.h
#ifndef BIT7MASK
#define BIT7MASK              0x80
#endif
#ifndef LO_NIBBLE_MASK
#define LO_NIBBLE_MASK        0x0f
#endif

#include "a7rc_check.h"

// ---- Limits ---------------------------------------------------------------

// The link's width, x1 or x4: the wrapper's lanes (ChkInit())
static int chk_width = 1;

// Ack latency limit, Max_Payload_Size 128 bytes, in symbol times (3.5.3.1,
// Tables 3-6/3-7, x1 or x4: no tolerance, unlike the replay timer), plus what
// the spec has a measurement allow: a TLP already in progress (the largest,
// CHK_TLP_MAX_SYM, striped over the lanes) and the Ack DLLP itself (8)
#define CHK_TLP_MAX_SYM       ((152 + chk_width - 1) / chk_width)   // MPS 128: STP, 2 + 16 + 128 + 4, END
#define CHK_ACK_LAT_GEN1_SYM  ((chk_width == 4 ? 73  : 237) + CHK_TLP_MAX_SYM + 8)
#define CHK_ACK_LAT_GEN2_SYM  ((chk_width == 4 ? 124 : 288) + CHK_TLP_MAX_SYM + 8)

// The replay timer limit, Max_Payload_Size 128 bytes, in symbol times
// (3.5.2.1, Tables 3-4/3-5, x1 or x4), -0%/+100%: an endpoint may replay a
// TLP without a Nak once this long has passed since it sent it
#define CHK_REPLAY_GEN1_SYM   (chk_width == 4 ? 219 : 711)
#define CHK_REPLAY_GEN2_SYM   (chk_width == 4 ? 372 : 864)
// Margin for the endpoint's replay: its late bound (twice the timer) allows
// for the link's round trip and one TLP
#define CHK_REPLAY_SLACK_NS   (CHK_LINK_RT_NS + CHK_TLP_MAX_SYM * 4)

// The Ack latency limit in ns, for a TLP sent at Gen 'tx' and Acked at Gen 'now'
#define CHK_ACK_LIMIT_NS(tx, now) ((((tx) == 2 && (now) == 2) ? CHK_ACK_LAT_GEN2_SYM * 2 : CHK_ACK_LAT_GEN1_SYM * 4) + CHK_LINK_RT_NS)

// The link both ways, which the RC model sees on top of the endpoint's own
// Ack latency: its TLP's END goes out, the Ack comes back. On the serial
// line, the SerDes BFM and Cologne Chip's PIPE logic add about 0.5 us
#ifdef SIM_SERIAL
#define CHK_LINK_RT_NS        1000
#else
#define CHK_LINK_RT_NS        100
#endif

// UpdateFC for each finite type at least every 30 us, -0%/+50% (2.6.1.2)
#define CHK_UPDATEFC_NS       45000

// The RC model leaves Device Control at its defaults: Max_Payload_Size 128
// bytes, Max_Read_Request_Size 512 bytes (7.8.4). An endpoint completer
// splits reads on 128 bytes (2.3.1.1: the RCB of all but a Root Complex)
#define CHK_MPS_BYTES         128
#define CHK_MRRS_BYTES        512
#define CHK_RCB_BYTES         128


#define CHK_PRINT_MAX         10        // violations printed per rule
#define CHK_SKP_MIN_SYM       1180      // SKP Ordered Set interval (4.2.7.1), on average
#define CHK_SKP_MAX_SYM       1538
#define CHK_SKP_GAP_SYM       (CHK_SKP_MAX_SYM + CHK_MPS_BYTES + 28)   // the longest gap a receiver takes (4.2.7.2)
#define CHK_LTSSM_DETECT      0         // GetLtssmState() (ltssm.c)
#define CHK_LTSSM_POLLING     1
#define CHK_LTSSM_CONFIG      2
#define CHK_LTSSM_RECOVERY    3
#define CHK_LTSSM_L0          7
#define CHK_PAD               0x1f7     // K23.7: Link or Lane Number not set
#define CHK_EIOS_IDLE_NS      800       // Recovery.Speed: the shortest electrical idle (4.2.6.4.2)
#define CHK_EIOS_FAIL_NS      6000      // ... after a failed speed negotiation
#define CHK_RCVRLOCK_NS       23900000  // Recovery.RcvrLock times out, to Recovery.Speed: 24 ms (4.2.6.4.1),
                                        // less the time the RC takes to see the Recovery (receiver lock)
#define CHK_SC_TS2            32        // TS2s with speed_change before Recovery.Speed (4.2.6.4.3)

// ---- Rules ----------------------------------------------------------------

enum
{
    R_FMT_TYPE, R_RSVD, R_LENGTH, R_ECRC, R_POISON, R_REQ_ID, R_REQ_ADDR, R_REQ_4KB, R_REQ_BE,
    R_CPL_MATCH, R_CPL_ID, R_CPL_STATUS, R_CPL_FIELDS, R_CPL_TC_ATTR, R_CPL_ALL,
    R_DLLP_CRC, R_DLLP_TYPE, R_NAK, R_LCRC, R_SEQ, R_REPLAY, R_ACK_SEQ, R_ACK_LAT,
    R_FC_ORDER, R_FC_SAME, R_FC_INF, R_FC_FWD, R_FC_RANGE, R_FC_PERIOD, R_TLP_EARLY, R_EP_CREDITS,
    R_CFG_ID, R_CFG_HEADER, R_CFG_CAPS, R_CFG_PCIE_CAP, R_CFG_LINK, R_CFG_EXT_CAPS, R_CFG_BAR, R_CFG_RO,
    R_CFG_RW1C, R_CFG_FN, R_ORDER, R_DLLP_RSVD, R_FC_INIT_MIN, R_FC_CPL_INF, R_FC_RETURN, R_FRAMING,
    R_UR, R_CRS, R_ERR_STATUS, R_REPLAY_ORDER, R_BAD_TLP, R_DUPLICATE, R_REQ_BME,
    R_PL_L0_ONLY, R_PL_SPEED, R_PL_TS_FIELDS, R_PL_SKP, R_PL_EIOS,
    R_NUM
};

struct Rule
{
    const char *id;
    const char *sec;                    // PCIe Base 2.1
    const char *what;
    unsigned    checks = 0;
    unsigned    fails  = 0;
};

static Rule rules[R_NUM] =
{
    {"TL.FMT-TYPE",     "2.2.1",   "Fmt/Type defined for an endpoint: no MRdLk, no Cfg, no I/O (but a Legacy Endpoint); Message routing it may use"},
    {"TL.RSVD",         "2.2.1",   "reserved header bits 0; TH, IDO, AT 0 (not enabled)"},
    {"TL.LENGTH",       "2.2.2",   "Length matches the payload; payload <= MPS, read <= MRRS"},
    {"TL.ECRC",         "2.7.1",   "no ECRC (TD 0): the endpoint has no AER, so ECRC generation is off, also for requests with ECRC"},
    {"TL.POISON",       "2.7.2.2",   "EP 1 only on a TLP with data"},
    {"TL.REQ-ID",       "2.2.6.2", "requests carry the captured bus/device, function 0"},
    {"TL.REQ-ADDR",     "2.2.4.1", "address bits 1:0 0; 64-bit format only above 4 GB"},
    {"TL.REQ-4KB",      "2.2.7",   "no memory request crosses a 4 KB boundary"},
    {"TL.REQ-BE",       "2.2.5",   "First/Last DW Byte Enables legal for the Length"},
    {"TL.CPL-MATCH",    "2.3.2",   "Completion for an outstanding request: Requester ID, Tag"},
    {"TL.CPL-ID",       "2.2.9", "Completer ID: the captured bus/device, function addressed"},
    {"TL.CPL-STATUS",   "2.2.9",   "status defined; CRS only for Cfg; data only with SC, for reads"},
    {"TL.CPL-FIELDS",   "2.2.9",   "Byte Count, Lower Address, Length; BCM 0; a split ends on the completer's RCB, 128 bytes (2.3.1.1)"},
    {"TL.CPL-TC-ATTR",  "2.2.9",   "Completion TC and Attr as the request's"},
    {"TL.CPL-ALL",      "2.3.1",   "every non-posted request completed"},
    {"DL.DLLP-CRC",     "3.4.1",   "DLLP CRC good"},
    {"DL.DLLP-TYPE",    "3.4.1",   "DLLP type defined, VC0 only"},
    {"DL.NAK",          "3.5.3.1", "no Nak: the line is error-free, but for the RC's injected faults"},
    {"DL.LCRC",         "3.5.2.1", "TLP LCRC good, or inverted with EDB"},
    {"DL.SEQ",          "3.5.2.1", "TLP sequence numbers from 0, one up per new TLP"},
    {"DL.REPLAY",       "3.5.2.1", "a replay only after an RC Nak, or once the replay timer expired, and by twice the timer"},
    {"DL.ACK-SEQ",      "3.5.2.2", "Ack/Nak names a TLP sent, never an older one"},
    {"DL.ACK-LAT",      "3.5.3.1", "each TLP Acked within the Ack latency limit (no tolerance), plus a TLP in progress and the link"},
    {"FC.INIT-ORDER",   "3.3.1",   "InitFC1, then InitFC2, then UpdateFC; each set P, NP, Cpl in order; UpdateFC once FC_INIT2 can be done"},
    {"FC.INIT-SAME",    "2.6.1.2",   "InitFC2 credits equal InitFC1"},
    {"FC.INFINITE",     "2.6.1",   "UpdateFC of an infinite credit type carries 0"},
    {"FC.FORWARD",      "2.6.1.2",   "UpdateFC never moves a credit limit back"},
    {"FC.RANGE",        "2.6.1",   "at most 127 header and 2047 data credits ahead of the RC"},
    {"FC.UPDATE-PERIOD","2.6.1.2", "UpdateFC of each finite type at most 30 us +50% apart"},
    {"FC.TLP-EARLY",    "3.3.1",   "no TLP before the endpoint left FC_INIT2: its InitFC2 sent, or the RC's received"},
    {"FC.EP-CREDITS",   "2.6.1.1",   "the endpoint's TLPs fit the RC's credits"},
    {"CFG.ID",          "7.5.1",   "Vendor ID neither FFFFh nor 0000h"},
    {"CFG.HEADER",      "7.5.1.2",   "Header Type 0, Latency Timer 0, Capabilities List 1, Capabilities Pointer >= 40h; reserved 0"},
    {"CFG.CAP-LIST",    "7.6",   "capability chain ends, no loop; PM and PCIe Capabilities present"},
    {"CFG.PCIE-CAP",    "7.8.2",   "PCIe Capability version 2 (1 only for a 2.5 GT/s device), Endpoint, no slot; MPS Supported defined; Role-Based Error Reporting (7.8.3)"},
    {"CFG.LINK",        "7.8.6",   "Link Capabilities speed/width defined; Link Status = the trained link"},
    {"CFG.EXT-CAPS",    "7.9.1",     "extended capability chain from 100h ends (all 0 if none)"},
    {"CFG.BAR",         "7.5.2.1", "BAR sizing read: type defined, size mask contiguous"},
    {"CFG.RO",          "7.4",     "read-only bits keep their value across a write"},
    {"CFG.RW1C",        "7.4",     "RW1C bits that were 1: 0 after a write of 1, still 1 after a write of 0"},
    {"CFG.FN",          "7.3.3",     "a request to a function a single-function device lacks gets UR"},
    {"TL.ORDER",        "2.4.1",     "a read returns what the RC wrote before it (or after it: a write may pass)"},
    {"DL.DLLP-RSVD",    "3.4.1",     "DLLP reserved fields 0"},
    {"FC.INIT-MIN",     "2.6.1",   "initial data credits: P and Cpl >= Max_Payload_Size Supported, NP >= 1 (or infinite)"},
    {"FC.CPL-INF",      "2.6.1",   "an endpoint advertises infinite Completion credits"},
    {"FC.RETURN",       "2.6.1.2",   "by the end, UpdateFC returned every credit the RC's TLPs used (an expectation: the run idles 45 us first)"},
    {"PL.FRAMING",      "4.2.2",   "no K symbol inside a packet; a DLLP is 8 symbols and ends in END"},
    {"TL.UR",           "2.3.1",   "an unsupported non-posted request gets UR, a supported one never"},
    {"TL.CRS",          "2.3.1",   "CRS only until the first successful Configuration Completion"},
    {"CFG.ERR-STATUS",  "6.2.5",   "Device Status after an error, Figure 6-2 (no AER): a posted UR Non-Fatal and UR Detected; an Advisory one (non-posted UR, RBER) no severity; Bad TLP/DLLP, replay timeout Correctable"},
    {"DL.REPLAY-ORDER", "3.5.2.1", "a replay starts at the oldest unAcked TLP (after a Nak, the one after its), in order, before any new TLP"},
    {"DL.BAD-TLP",      "3.5.3.1", "a TLP with a bad LCRC is Naked with the last good sequence number, and dropped"},
    {"DL.DUPLICATE",    "3.5.3.1", "a duplicate TLP (sent again after its Ack) is Acked within the Ack latency limit, not Naked, and dropped"},
    {"TL.REQ-BME",      "7.5.1.1", "the endpoint's memory and I/O requests only with Bus Master Enable set"},
    {"PL.L0-ONLY",      "4.2.6.4", "no TLP or DLLP from the endpoint's first TS1/TS2 of a training or Recovery until it can be in L0: the RC back in L0, or 16 symbol times and a DLLP after the RC began Configuration.Idle or Recovery.Idle"},
    {"PL.SPEED",        "4.2.6.4.3", "a speed change up only in a Recovery whose TS2s from the endpoint carried speed_change, to a rate they advertised (down to 2.5 GT/s: also the fallback, 4.2.6.4.2)"},
    {"PL.TS-FIELDS",    "4.2.4.1", "TS1/TS2: Data Rate bit 1 set, bits 0, 3-5 0, speed_change not in Polling or Configuration, the same rates there; Training Control 0 but Disable Scrambling (bits 5-7 reserved; the RC directs no Hot Reset, Disable, Loopback or Compliance Receive); after Configuration, the Link and Lane Numbers it set"},
    {"PL.SKP",          "4.2.7.1", "SKP Ordered Sets, lane 0: in Polling, Configuration and L0 never more than 1538 + Max_Payload_Size + 28 Symbol Times apart (4.2.7.2); in L0 one per 1180 to 1538 (give or take one per L0 period)"},
    {"PL.EIOS",         "4.2.4.2", "EIOS from the endpoint, lane 0: only in Recovery, on the way to Recovery.Speed (after TS2s with speed_change, at 2.5 GT/s with 5.0 GT/s on both sides; or, at 5.0 GT/s or back from a changed rate, after RcvrLock timed out with TS1 only, 4.2.6.4.1, or in RcvrCfg, after its TS2, once the RC sent EIOS, 4.2.6.4.3); one at 2.5 GT/s, two in a row at 5.0 GT/s; at least 32 TS2s with speed_change before (counted from its first); then electrically idle until the RC is, and for at least 800 ns after the later EIOS, 6 us without speed_change (4.2.6.4.2)"},
};

static int PcieCapDw(void);

// ---- State ----------------------------------------------------------------

static int      chk_node = -1;

// Endpoint TLPs: the next new sequence number; an RC Nak since the last one
static int      ep_next_seq   = 0;
static bool     rc_nak_since  = false;

// RC TLPs: the last new sequence number sent, the last one Acked, and when
// each pending one went out
static int      rc_last_seq   = 0xfff;
static int      rc_acked      = 0xfff;
static uint64_t rc_tx_ns[4096];
static int      rc_tx_gen[4096];        // the speed it went out at
static uint64_t ep_tx_ns[4096];         // endpoint TLPs: when each last went out
static int      ep_acked      = 0xfff;  // ... the last the RC Acked, and when that advanced
static uint64_t ep_ack_ns     = 0;
static int      ep_last_rpl   = -1;     // ... the last one sent again
static bool     rc_nak_owed   = false;  // the RC Naked, the replay not yet started
static uint64_t rc_nak_ns     = 0;      // ... when
static int      ep_acked_prev = 0xfff;  // the RC's Acks: the one before the last advance
static bool     rc_init2_sent = false;  // the RC sent InitFC2: the endpoint may leave FC_INIT2
static bool     corr_since    = false;  // a Correctable error at the endpoint since Device Status was read
static bool     poison_np     = false;  // a poisoned non-posted request: its UR, and maybe Non-Fatal too
static unsigned rc_naks       = 0;      // for the report: the RC's Naks, the endpoint's replays
static unsigned ep_rpl_runs   = 0, ep_rpl_tlps = 0;

// The RC's injected faults (ChkFault()): armed, then the TLP they hit
static int      rc_fault_arm  = 0;
static int      rc_bad_seq    = -1;     // a TLP sent with a bad LCRC: a Nak is due
static int      rc_dup_seq    = -1;     // a TLP sent twice: an Ack is due
static uint64_t rc_dup_ns     = 0;      // ... when it went out, at which speed
static int      rc_dup_gen    = 1;

// The endpoint's own requests: Bus Master Enable as the RC last wrote it;
// requests seen, non-posted ones, and the RC's last Completions to them
static bool     ep_bme        = false;
static unsigned ep_reqs       = 0, ep_np = 0, ep_np_done = 0;
static uint32_t ep_post_hdr_min = 0xff; // the fewest posted header credits the endpoint had left
static bool     rc_pending[4096];

// Flow control, per type (FC_POST, FC_NONPOST, FC_CMPL): what the endpoint
// advertised, at InitFC1 and since; what the RC consumed of it; and the
// other way round
static int      ep_fc_kind     = 0;     // 0 none yet, 1 InitFC1, 3 InitFC2, 2 UpdateFC
static int      ep_fc_next     = 0;     // the type expected next in a set
static bool     ep_init1_valid[FC_NUMTYPES];
static uint32_t ep_init1_hdr[FC_NUMTYPES], ep_init1_data[FC_NUMTYPES];
static uint32_t ep_lim_hdr[FC_NUMTYPES],   ep_lim_data[FC_NUMTYPES];
static uint64_t ep_fc_last_ns[FC_NUMTYPES];
static bool     ep_init2_seen  = false;
static uint32_t rc_cons_hdr[FC_NUMTYPES],  rc_cons_data[FC_NUMTYPES];

static bool     rc_fc_valid[FC_NUMTYPES];
static uint32_t rc_lim_hdr[FC_NUMTYPES],   rc_lim_data[FC_NUMTYPES];
static uint32_t ep_cons_hdr[FC_NUMTYPES],  ep_cons_data[FC_NUMTYPES];

// The RC's non-posted requests, by tag, until their last Completion
enum { REQ_MEM_RD, REQ_MEM_RDLK, REQ_IO_CFG_RD, REQ_IO_CFG_WR };

struct Req
{
    bool     live;
    int      kind;
    bool     cfg;                       // a Configuration Request: CRS allowed
    bool     cfg0;                      // Type 0, to function 0: the shadow below
    uint32_t reg, be, wdata;            // Cfg: the DW, First DW BE, the data written
    uint32_t rid, tc, attr;
    uint32_t fn;                        // the function addressed (Cfg), else 0
    uint32_t remaining;                 // memory reads: bytes still to come
    uint32_t addr;                      // memory reads: the next byte's address, low byte
    int      cap_bus, cap_dev;          // a Type 0 CfgWr: what the endpoint captures, else -1
    uint64_t start, next;               // memory reads: the first byte's address, the next one's
    std::vector<int> exp;               // memory reads, per byte: the data written before (-1 none,
                                        // or a byte the read did not enable)
    std::vector<std::vector<uint8_t>> alt;  // and every value written after it (a write may pass the read)
    bool     expect_ur;                 // the stimulus sent it as an Unsupported Request
};

static Req      req[256];
static unsigned req_gen[256];           // non-posted requests sent with each tag
static bool     ur_next[256];           // the next request with this tag is an Unsupported Request
static int      ur_since    = 0;        // Unsupported Requests since the last Device Status read: UR_NP, UR_P
enum { UR_NP = 1, UR_P = 2 };
static bool     ur_clear    = false;    // UR Detected read 0, and no Unsupported Request since
static bool     cfg_sc_seen = false;    // a Configuration Request completed successfully

// The endpoint's bus/device numbers, captured from Type 0 CfgWr (2.2.6.2)
static int      ep_bus = -1, ep_dev = -1;

// Function 0's configuration space, by DW, as read: the first value and the
// link speed then (structure checks), the last value; a write since the last
// read, with the value before it
#define CFG_DWS               1024
static bool     cfg_seen[CFG_DWS];
static uint32_t cfg_first[CFG_DWS], cfg_first_speed[CFG_DWS], cfg_val[CFG_DWS];
static bool     cfg_wr_pend[CFG_DWS], cfg_before_ok[CFG_DWS];
static uint32_t cfg_wr_data[CFG_DWS], cfg_wr_be[CFG_DWS], cfg_before[CFG_DWS];

// Memory as the RC wrote it, by byte address (TL.ORDER); and what a poisoned
// write may have left there (its data may or may not be used, 2.7.2.2)
static std::map<uint64_t, uint8_t> mem;
static std::map<uint64_t, std::vector<uint8_t>> maybe;
static bool     poison_status = false;  // a poisoned write since the last read of Status
static bool     poison_dev    = false;  // ... and of Device Status

// The model's own packets that broke the framing rules
static int      model_errors = 0;

// ---- Helpers --------------------------------------------------------------

static uint64_t Now(void)
{
    return GetLinkTimeNs(chk_node);
}

// Link time in L0: the Ack latency and UpdateFC timers stop in Recovery
static uint64_t L0Now(void)
{
    return GetLinkTimeNs(chk_node) - GetLinkRecoveryNs(chk_node);
}

// Counts one check of rule 'r'; on failure prints the details
static void Check(const int r, const bool ok, const char *fmt, ...)
{
    rules[r].checks++;
    if (ok)
    {
        return;
    }
    if (rules[r].fails++ < CHK_PRINT_MAX)
    {
        char    msg[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        VPrint("**** CHECK %s (%s) at %.3f us: %s\n", rules[r].id, rules[r].sec, Now() / 1000.0, msg);
    }
}

// a is at most 'half' ahead of b, modulo 'mod': a >= b in sequence space
static bool NotBehind(const uint32_t a, const uint32_t b, const uint32_t mod)
{
    return ((a - b) & (mod - 1)) <= mod / 2;
}

// A TLP's flow control type, and the data credits its payload uses
static int TlpFcType(const PktData_t *d)
{
    int ft = d[TLP_TYPE_BYTE_OFFSET] & 0x7f;
    int t  = ft & 0x1f;

    if ((t & 0x18) == 0x10 || (t == 0 && (ft & TL_TYPE_WRITE)))
    {
        return FC_POST;                         // Msg, MsgD, MWr
    }
    if (t == 0x0a || t == 0x0b)
    {
        return FC_CMPL;                         // Cpl, CplD, CplLk, CplDLk
    }
    return FC_NONPOST;                          // MRd, MRdLk, IO, Cfg
}

// Length in DW: 0 is 1024 (2.2.2)
static uint32_t Len(const PktData_t *d)
{
    uint32_t n = GET_TLP_LENGTH(d);
    return n ? n : 1024;
}

static uint32_t TlpDataCredits(const PktData_t *d)
{
    if (!(d[TLP_TYPE_BYTE_OFFSET] & TL_TYPE_WRITE))
    {
        return 0;
    }
    return (Len(d) * 4 + FC_DATA_CREDIT_BYTES - 1) / FC_DATA_CREDIT_BYTES;
}

// The packet's symbols, to its PKT_TERMINATION, for a CRC calculation
static std::vector<PktData_t> Copy(const PktData_t *d)
{
    std::vector<PktData_t> v;
    for (int i = 0; d[i] != PKT_TERMINATION; i++)
    {
        v.push_back(d[i]);
    }
    v.push_back(PKT_TERMINATION);
    return v;
}

static void FcFields(const PktData_t *d, uint32_t *hdr, uint32_t *data)
{
    *hdr  = ((d[2] & 0x3f) << 2) | ((d[3] >> 6) & 0x3);
    *data = ((d[3] & 0x0f) << 8) | (d[4] & 0xff);
}

// ---- Framing --------------------------------------------------------------

// A packet's symbols: STP (TLP) or SDP (DLLP) first, END (or EDB, a TLP) last,
// no K symbol between; a DLLP is 8 symbols. Returns what is wrong, or NULL
static const char *Framing(const PktData_t *d, const bool tlp)
{
    int n = 0;
    while (d[n] != PKT_TERMINATION)
    {
        n++;
    }
    if (n < 2 || d[0] != (tlp ? STP : SDP))
    {
        return "no STP/SDP first";
    }
    if (!(d[n - 1] == END || (tlp && d[n - 1] == EDB)))
    {
        return "no END/EDB last";
    }
    for (int i = 1; i < n - 1; i++)
    {
        if (d[i] & 0x100)
        {
            return "a K symbol inside";
        }
    }
    if (!tlp && n != 8)
    {
        return "a DLLP not 8 symbols";
    }
    return NULL;
}

// ---- Endpoint DLLPs -------------------------------------------------------

static void RxAckNak(const bool nak, const int seq)
{
    Check(R_NAK, !nak || rc_bad_seq >= 0, "Nak, sequence number %d", seq);

    // A bad TLP: Naked with the sequence number before it, not Acked
    if (rc_bad_seq >= 0 && (nak || NotBehind(seq, rc_bad_seq, 4096)))
    {
        Check(R_BAD_TLP, nak && seq == ((rc_bad_seq - 1) & 0xfff), "TLP %d sent with a bad LCRC: the endpoint's %s %d",
              rc_bad_seq, nak ? "Nak" : "Ack", seq);
        rc_bad_seq = -1;
    }
    // A duplicate: Acked (it names the last TLP received, at or past it)
    else if (rc_dup_seq >= 0 && (nak || NotBehind(seq, rc_dup_seq, 4096)))   // sent after its Ack: this one is the duplicate's
    {
        uint64_t lat = L0Now() - rc_dup_ns, lim = CHK_ACK_LIMIT_NS(rc_dup_gen, (int)GetLinkSpeed(chk_node));
        Check(R_DUPLICATE, !nak && lat <= lim, "TLP %d sent again after its Ack: the endpoint's %s %d after %llu ns, limit %llu ns",
              rc_dup_seq, nak ? "Nak" : "Ack", seq, (unsigned long long)lat, (unsigned long long)lim);
        rc_dup_seq = -1;
    }

    Check(R_ACK_SEQ, NotBehind(rc_last_seq, seq, 4096) && NotBehind(seq, rc_acked, 4096),
          "%s %d: the RC sent up to %d, Acked up to %d", nak ? "Nak" : "Ack", seq, rc_last_seq, rc_acked);

    // Ack and Nak both acknowledge up to 'seq'
    if (seq == rc_acked || !NotBehind(seq, rc_acked, 4096))
    {
        return;
    }
    uint32_t gen   = GetLinkSpeed(chk_node);
    for (int s = (rc_acked + 1) & 0xfff; ; s = (s + 1) & 0xfff)
    {
        if (rc_pending[s])
        {
            uint64_t limit = CHK_ACK_LIMIT_NS(rc_tx_gen[s], (int)gen);
            uint64_t lat   = L0Now() - rc_tx_ns[s];
            Check(R_ACK_LAT, lat <= limit, "TLP %d Acked after %llu ns in L0, limit %llu ns at Gen%u",
                  s, (unsigned long long)lat, (unsigned long long)limit, gen);
            rc_pending[s] = false;
        }
        if (s == seq)
        {
            break;
        }
    }
    rc_acked = seq;
}

static void RxFc(const int type, const PktData_t *d)
{
    int      kind = (type >> 6) & 3;            // 1 InitFC1, 3 InitFC2, 2 UpdateFC
    int      t    = (type >> 4) & 3;            // FC_POST, FC_NONPOST, FC_CMPL
    uint32_t hdr, data;
    FcFields(d, &hdr, &data);

    // Order: InitFC1 sets, InitFC2 sets, UpdateFCs; each set P, NP, Cpl
    static const char *kind_name[4] = {"nothing", "InitFC1", "UpdateFC", "InitFC2"};
    const char *prev     = kind_name[ep_fc_kind];
    bool        order_ok = true;
    if (kind != 2)
    {
        bool from_ok = (ep_fc_kind == kind) || (ep_fc_kind == 0 && kind == 1) ||
                       (ep_fc_kind == 1 && kind == 3);       // FI1 may come mid-set
        if (ep_fc_kind != kind)
        {
            ep_fc_next = 0;
        }
        order_ok   = from_ok && t == ep_fc_next;
        ep_fc_next = (t + 1) % 3;
        ep_fc_kind = kind;
    }
    else
    {
        order_ok   = ep_init2_seen || rc_init2_sent;
        ep_fc_kind = 2;
    }
    Check(R_FC_ORDER, order_ok, "%s type %d after %s", kind_name[kind], t, prev);

    if (kind == 1)
    {
        if (t == FC_CMPL)
        {
            Check(R_FC_CPL_INF, hdr == FC_INFINITE_CREDITS && data == FC_INFINITE_CREDITS, "InitFC1-Cpl %u/%u", hdr, data);
        }
        ep_init1_valid[t] = true;
        ep_init1_hdr[t]   = ep_lim_hdr[t]  = hdr;
        ep_init1_data[t]  = ep_lim_data[t] = data;
        return;
    }

    if (kind == 3)
    {
        Check(R_FC_SAME, ep_init1_valid[t] && hdr == ep_init1_hdr[t] && data == ep_init1_data[t],
              "InitFC2 type %d: %u/%u, InitFC1 %u/%u", t, hdr, data, ep_init1_hdr[t], ep_init1_data[t]);
        ep_init2_seen = true;
    }
    else
    {
        // UpdateFC: 0 stays 0 for an infinite type; a finite limit only moves on
        bool hdr_inf  = ep_init1_hdr[t] == FC_INFINITE_CREDITS;
        bool data_inf = ep_init1_data[t] == FC_INFINITE_CREDITS;
        Check(R_FC_INF, (!hdr_inf || hdr == 0) && (!data_inf || data == 0),
              "UpdateFC type %d %u/%u, InitFC %u/%u", t, hdr, data, ep_init1_hdr[t], ep_init1_data[t]);
        Check(R_FC_FWD, (hdr_inf || NotBehind(hdr, ep_lim_hdr[t], 256)) && (data_inf || NotBehind(data, ep_lim_data[t], 4096)),
              "UpdateFC type %d %u/%u after %u/%u", t, hdr, data, ep_lim_hdr[t], ep_lim_data[t]);
        Check(R_FC_RANGE, (hdr_inf || ((hdr - rc_cons_hdr[t]) & 0xff) <= 127) &&
                          (data_inf || ((data - rc_cons_data[t]) & 0xfff) <= 2047),
              "UpdateFC type %d %u/%u, the RC consumed %u/%u", t, hdr, data, rc_cons_hdr[t] & 0xff, rc_cons_data[t] & 0xfff);
        if (!hdr_inf)
        {
            ep_lim_hdr[t] = hdr;
        }
        if (!data_inf)
        {
            ep_lim_data[t] = data;
        }
    }

    // Period: since the last InitFC2 or UpdateFC of the type
    bool finite = ep_init1_hdr[t] != FC_INFINITE_CREDITS || ep_init1_data[t] != FC_INFINITE_CREDITS;
    if (finite && ep_fc_last_ns[t])
    {
        uint64_t gap = L0Now() - ep_fc_last_ns[t];
        Check(R_FC_PERIOD, gap <= CHK_UPDATEFC_NS, "UpdateFC type %d %llu ns after the last", t, (unsigned long long)gap);
    }
    ep_fc_last_ns[t] = L0Now();
}

static void RxDllp(const PktData_t *d)
{
    const char *bad = Framing(d, false);
    Check(R_FRAMING, !bad, "DLLP: %s", bad ? bad : "");
    if (bad)
    {
        return;
    }

    std::vector<PktData_t> c = Copy(d);
    CalcDllpCrc(c.data());
    bool crc_ok = c[5] == d[5] && c[6] == d[6];
    Check(R_DLLP_CRC, crc_ok, "DLLP type 0x%02x, CRC %02x%02x, expected %02x%02x", d[1] & 0xff, d[5] & 0xff, d[6] & 0xff,
          c[5] & 0xff, c[6] & 0xff);
    if (!crc_ok)
    {
        return;
    }

    int  type    = d[1] & 0xff;
    bool fc      = (type & 0x40) || (type & 0x80);
    int  base    = fc ? (type & 0xf8) : type;
    bool defined = base == DL_ACK || base == DL_NAK || base == DL_VENDOR ||
                   base == 0x20 || base == 0x21 || base == 0x23 || base == 0x24 ||   // PM; 22h is reserved
                   base == DL_INITFC1_P || base == DL_INITFC1_NP || base == DL_INITFC1_CPL ||
                   base == DL_INITFC2_P || base == DL_INITFC2_NP || base == DL_INITFC2_CPL ||
                   base == DL_UPDATEFC_P || base == DL_UPDATEFC_NP || base == DL_UPDATEFC_CPL;
    Check(R_DLLP_TYPE, defined && (!fc || (type & DL_VC_BITS) == 0), "DLLP type 0x%02x", type);

    // Reserved: Ack/Nak byte 1 and the top of byte 2; FC bits 7:6 of byte 1,
    // 5:4 of byte 2; PM bytes 1-3
    uint32_t rsvd = (base == DL_ACK || base == DL_NAK)                ? ((d[2] & 0xff) | (d[3] & 0xf0))
                  : fc                                                ? ((d[2] & 0xc0) | (d[3] & 0x30))
                  : (base == 0x20 || base == 0x21 || base == 0x23 || base == 0x24) ? ((d[2] | d[3] | d[4]) & 0xff)
                  : 0;
    Check(R_DLLP_RSVD, rsvd == 0, "DLLP type 0x%02x: bytes 1-3 %02x %02x %02x", type, d[2] & 0xff, d[3] & 0xff, d[4] & 0xff);

    if (base == DL_ACK || base == DL_NAK)
    {
        RxAckNak(base == DL_NAK, ((d[3] & 0xf) << 8) | (d[4] & 0xff));
    }
    else if (fc && defined && (type & DL_VC_BITS) == 0)
    {
        RxFc(base, d);
    }
}

// ---- Transaction Layer ----------------------------------------------------

// TLP header byte 'k' (byte 0: Fmt/Type)
static uint32_t H(const PktData_t *d, const int k)
{
    return d[TLP_TYPE_BYTE_OFFSET + k] & 0xff;
}

// Byte Enable helpers: the lowest enabled byte, and the bytes a read asks
// for (2.2.9, its rules for Byte Count)
static int BeLow(const uint32_t be)
{
    int n = 0;
    while (n < 4 && !((be >> n) & 1))
    {
        n++;
    }
    return n;
}

static int BeHigh(const uint32_t be)
{
    int n = 3;
    while (n >= 0 && !((be >> n) & 1))
    {
        n--;
    }
    return n;
}

static uint32_t ReadBytes(const uint32_t len, const uint32_t fbe, const uint32_t lbe)
{
    if (len == 1)
    {
        return fbe ? BeHigh(fbe) - BeLow(fbe) + 1 : 1;
    }
    return len * 4 - BeLow(fbe) - (3 - BeHigh(lbe));
}

// An RC memory write: into the shadow, and into the reads still waiting that
// it overlaps (a posted write may pass a read, 2.4)
static void TxMemWrite(const PktData_t *d)
{
    uint64_t         base = GET_TLP_ADDRESS(d) & ~3ull;
    uint32_t         len  = Len(d), fbe = GET_TLP_FBE(d), lbe = GET_TLP_LBE(d);
    const PktData_t *p    = GET_TLP_PAYLOAD_PTR(d);
    bool             pois = H(d, 2) & 0x40;

    for (uint32_t i = 0; i < len * 4; i++)
    {
        uint32_t be = (i < 4) ? fbe : (len > 1 && i >= (len - 1) * 4) ? lbe : 0xf;
        if (!((be >> (i & 3)) & 1))
        {
            continue;
        }
        uint64_t a = base + i;
        if (pois)
        {
            maybe[a].push_back(p[i] & 0xff);
        }
        else
        {
            mem[a] = p[i] & 0xff;
            maybe.erase(a);
        }
        for (Req &r : req)
        {
            if (r.live && r.kind == REQ_MEM_RD && a >= r.next && a < r.start + r.exp.size())
            {
                r.alt[a - r.start].push_back(p[i] & 0xff);
            }
        }
    }
}

// An RC request: a non-posted one waits for its Completions
static void TxRequest(const PktData_t *d)
{
    uint32_t type = H(d, 0) & 0x1f;
    bool     wr   = H(d, 0) & TL_TYPE_WRITE;
    int      kind;

    // A poisoned request: Detected Parity Error, always; logged as Poisoned TLP
    // Received for a posted one only, a non-posted one being an Unsupported
    // Request here, which takes precedence (6.2) (CFG.ERR-STATUS)
    if (H(d, 2) & 0x40)
    {
        poison_status = true;
        poison_np    |= !((type == 0x00 && wr) || (type & 0x18) == 0x10);
        poison_dev   |= (type == 0x00 && wr) || (type & 0x18) == 0x10;
    }

    if (type == 0x00 && !wr)
    {
        kind = REQ_MEM_RD;
    }
    else if (type == 0x01)
    {
        kind = REQ_MEM_RDLK;
    }
    else if (type == 0x02 || type == 0x04 || type == 0x05)
    {
        kind = wr ? REQ_IO_CFG_WR : REQ_IO_CFG_RD;
    }
    else
    {
        if (type == 0x00 && wr)
        {
            TxMemWrite(d);
        }
        return;                                 // posted
    }

    Req &r    = req[H(d, 6)];
    Check(R_CPL_ALL, !r.live, "tag %u reused while its request is outstanding: that one never completed", H(d, 6));
    req_gen[H(d, 6)]++;
    r.live      = true;
    r.expect_ur = ur_next[H(d, 6)];
    ur_next[H(d, 6)] = false;
    r.kind    = kind;
    r.cfg     = type == 0x04 || type == 0x05;
    r.rid     = (H(d, 4) << 8) | H(d, 5);
    r.tc      = (H(d, 1) >> 4) & 7;
    r.attr    = (H(d, 2) >> 4) & 3;
    if (kind == REQ_IO_CFG_WR && type == 0x04 && !(H(d, 9) & 7) && (((H(d, 10) & 0xf) << 6) | (H(d, 11) >> 2)) == 1 &&
        (H(d, 12) & 4))
    {
        ep_bme = true;                          // the endpoint may act on it before its Completion
    }
    r.fn      = r.cfg ? H(d, 9) & 7 : 0;
    r.cfg0    = type == 0x04 && r.fn == 0;
    r.reg     = ((H(d, 10) & 0xf) << 6) | (H(d, 11) >> 2);
    r.be      = GET_TLP_FBE(d);
    r.wdata   = (r.cfg && wr) ? ((d[TLP_DATA_OFFSET32] & 0xff) | ((d[TLP_DATA_OFFSET32 + 1] & 0xff) << 8) |
                                 ((d[TLP_DATA_OFFSET32 + 2] & 0xff) << 16) | ((uint32_t)(d[TLP_DATA_OFFSET32 + 3] & 0xff) << 24))
                              : 0;
    r.cap_bus = (type == 0x04 && wr) ? (int)H(d, 8) : -1;
    r.cap_dev = (type == 0x04 && wr) ? (int)(H(d, 9) >> 3) : -1;
    if (kind == REQ_MEM_RD || kind == REQ_MEM_RDLK)
    {
        uint32_t fbe = GET_TLP_FBE(d);
        r.remaining  = ReadBytes(Len(d), fbe, GET_TLP_LBE(d));
        r.addr       = (H(d, TLP_HDR_4DW(d) ? 15 : 11) & 0xfc) | (fbe ? BeLow(fbe) : 0);
        r.start      = (GET_TLP_ADDRESS(d) & ~3ull) + (fbe ? BeLow(fbe) : 0);
        r.next       = r.start;
        r.exp.assign(r.remaining, -1);
        r.alt.assign(r.remaining, {});
        uint32_t len  = Len(d), lbe = GET_TLP_LBE(d);
        uint64_t dw0  = GET_TLP_ADDRESS(d) & ~3ull;
        for (uint32_t i = 0; i < r.remaining; i++)
        {
            uint64_t a  = r.start + i;
            uint32_t dw = (a - dw0) / 4;
            uint32_t be = (dw == 0) ? fbe : (dw == len - 1) ? lbe : 0xf;
            auto     m  = mem.find(a);
            if (((be >> (a & 3)) & 1) && m != mem.end())
            {
                r.exp[i] = m->second;
                auto q   = maybe.find(a);
                if (q != maybe.end())
                {
                    r.alt[i] = q->second;
                }
            }
        }
    }
}

// Every new endpoint TLP: its header, and for requests the request rules.
// 'symbols' is its length to END (after the LCRC), 'lcrc_at' where the header
// says the LCRC starts
static void RxTlpHeader(const PktData_t *d, const uint32_t symbols, const uint32_t lcrc_at)
{
    uint32_t fmt  = H(d, 0) >> 5;
    uint32_t type = H(d, 0) & 0x1f;
    bool     data = fmt & 2;
    bool     msg  = (type & 0x18) == 0x10;
    uint32_t len  = Len(d);

    // Fmt/Type: no TLP Prefix (Fmt 4), routing 011 is the RC's broadcast,
    // 110 and 111 are reserved
    // An endpoint originates no MRdLk (6.5) and no Configuration Request (7.3)
    int  pcap    = PcieCapDw();
    bool legacy  = pcap >= 0 && cfg_seen[pcap] && ((cfg_first[pcap] >> 20) & 0xf) == 1;   // a Legacy Endpoint may (1.3.2.1)
    bool defined = (type == 0x00 && fmt <= 3) || (type == 0x02 && legacy && (fmt == 0 || fmt == 2)) ||
                   ((type == 0x0a || type == 0x0b) &&
                    (fmt == 0 || fmt == 2)) ||
                   (msg && (fmt == 1 || fmt == 3) && (type & 7) != 3 && (type & 7) < 6);
    Check(R_FMT_TYPE, defined, "Fmt %u Type 0x%02x", fmt, type);

    Check(R_RSVD, (H(d, 1) & 0x8f) == 0 && (H(d, 2) & 0x0c) == 0,
          "Fmt/Type 0x%02x: DW0 bytes 1-2 0x%02x%02x (reserved, IDO, TH, AT)", H(d, 0), H(d, 1), H(d, 2));

    Check(R_LENGTH, symbols == lcrc_at + 5 && (!data || len * 4 <= CHK_MPS_BYTES) &&
                    !((type == 0x00 || type == 0x01) && !data && len * 4 > CHK_MRRS_BYTES),
          "Fmt/Type 0x%02x, Length %u DW, %u symbols to END (header: %u)", H(d, 0), data ? len : 0, symbols,
          lcrc_at + 5);

    Check(R_ECRC, !TLP_HAS_DIGEST(d), "Fmt/Type 0x%02x with TD 1", H(d, 0));
    Check(R_POISON, !(H(d, 2) & 0x40) || data, "Fmt/Type 0x%02x poisoned (EP 1) without data", H(d, 0));

    if (type == 0x0a || type == 0x0b)
    {
        return;                                 // a Completion: RxCpl()
    }

    // The endpoint's own request: memory and I/O only with Bus Master Enable
    ep_reqs++;
    ep_np += (type <= 0x02 && !(H(d, 0) & TL_TYPE_WRITE)) || type == 0x02;
    if (type <= 0x02)
    {
        Check(R_REQ_BME, ep_bme, "Fmt/Type 0x%02x: Bus Master Enable 0", H(d, 0));
    }
    uint32_t rid = (H(d, 4) << 8) | H(d, 5);
    if (ep_bus >= 0)
    {
        Check(R_REQ_ID, rid == (uint32_t)((ep_bus << 8) | (ep_dev << 3)),
              "Fmt/Type 0x%02x, Requester ID 0x%04x, captured %02x:%02x.0", H(d, 0), rid, ep_bus, ep_dev);
    }
    if (msg)
    {
        return;                                 // byte 7 is the Message Code
    }

    uint32_t fbe = GET_TLP_FBE(d), lbe = GET_TLP_LBE(d);
    bool     be_ok;
    if (type != 0x00 && type != 0x01)
    {
        be_ok = len == 1 && lbe == 0;           // IO, Cfg: one DW
    }
    else if (len == 1)
    {
        be_ok = lbe == 0;
    }
    else
    {
        // Contiguous, except a 2 DW request in one aligned QW
        bool contig = (fbe == 0xf || fbe == 0xe || fbe == 0xc || fbe == 0x8) &&
                      (lbe == 0xf || lbe == 0x7 || lbe == 0x3 || lbe == 0x1);
        be_ok = fbe && lbe && (contig || (len == 2 && !(GET_TLP_ADDRESS(d) & 4)));
    }
    Check(R_REQ_BE, be_ok, "Fmt/Type 0x%02x, Length %u DW, First/Last DW BE 0x%x/0x%x", H(d, 0), len, fbe, lbe);

    if (type == 0x00 || type == 0x01)
    {
        uint64_t addr = GET_TLP_ADDRESS(d);
        Check(R_REQ_ADDR, !(H(d, TLP_HDR_4DW(d) ? 15 : 11) & 3) && (!TLP_HDR_4DW(d) || addr >> 32),
              "Fmt/Type 0x%02x, address 0x%llx", H(d, 0), (unsigned long long)addr);
        Check(R_REQ_4KB, (addr & 0xffc) + len * 4 <= 4096, "Fmt/Type 0x%02x, address 0x%llx, Length %u DW", H(d, 0),
              (unsigned long long)addr, len);
    }
}

// ---- Configuration space --------------------------------------------------

// The PCIe Capability's first DW, from the chain as first read; -1 if not
// (yet) found
static int PcieCapDw(void)
{
    if (!cfg_seen[0x0d])
    {
        return -1;
    }
    uint32_t ptr = cfg_first[0x0d] & 0xfc;
    for (int n = 0; n < 48 && ptr >= 0x40 && cfg_seen[ptr / 4]; n++)
    {
        if ((cfg_first[ptr / 4] & 0xff) == 0x10)
        {
            return ptr / 4;
        }
        ptr = (cfg_first[ptr / 4] >> 8) & 0xfc;
    }
    return -1;
}

// Read-only and RW1C bits per DW (7.5.1, 7.8): the IDs, Class Code,
// Subsystem IDs and Capabilities Pointer DWs; in Command/Status the reserved
// bits, Capabilities List, 66 MHz, Fast Back-to-Back, DEVSEL; Latency Timer
// and Header Type; the PCIe Capabilities, Device and Link Capabilities.
// RW1C: Status bits 15:11 and 8, Device Status bits 3:0
static uint32_t CfgRoMask(const int k)
{
    int p = PcieCapDw();
    switch (k)
    {
    case 0x00: case 0x02: case 0x0b: case 0x0d:
        return 0xffffffff;
    case 0x01:
        return 0x06f7f800;
    case 0x03:
        return 0x00ffff00;
    default:
        return (p >= 0 && (k == p || k == p + 1 || k == p + 3)) ? 0xffffffff : 0;
    }
}

static uint32_t CfgW1cMask(const int k)
{
    int p = PcieCapDw();
    return (k == 0x01) ? 0xf9000000 : (p >= 0 && k == p + 2) ? 0x000f0000 : 0;
}

// A mask of ones from bit 31 down, none below
static bool Contig(const uint32_t m)
{
    uint32_t x = ~m + 1;
    return m && !(x & (x - 1));
}

// A BAR read back after all ones were written to it (7.5.2.1)
static void CfgBarSize(const int k, const uint32_t v)
{
    bool upper = k > 4 && cfg_seen[k - 1] && (cfg_val[k - 1] & 7) == 4;   // of a 64-bit memory BAR
    bool ok;
    if (upper)
    {
        ok = v == 0 || Contig(v);
    }
    else if (v & 1)
    {
        uint32_t m = v & ~3u;                   // IO: may stop at bit 15
        ok = !(v & 2) && Contig(m | ((m & 0xffff0000) ? 0 : 0xffff0000));
    }
    else
    {
        uint32_t type = (v >> 1) & 3, m = v & ~0xfu;
        ok = v == 0 || ((type == 0 || type == 2) && (Contig(m) || (type == 2 && m == 0)));
    }
    Check(R_CFG_BAR, ok, "BAR at %03xh reads 0x%08x after all ones", k * 4, v);
}

// A Type 0 CfgWr to function 0 completed: the next read of the DW is checked
// against it
static void CfgWritten(const Req &r)
{
    int k              = r.reg;
    cfg_wr_pend[k]     = true;
    cfg_wr_data[k]     = r.wdata;
    cfg_wr_be[k]       = r.be;
    cfg_before[k]      = cfg_val[k];
    cfg_before_ok[k]   = cfg_seen[k];
    if (k == 1 && (r.be & 1))
    {
        ep_bme = (r.wdata >> 2) & 1;
    }
}

// A Type 0 CfgRd of function 0 completed with 'v'
static void CfgRead(const int k, const uint32_t v)
{
    // Device Status after an Unsupported Request: UR Detected, and without
    // Role-Based Error Reporting also Non-Fatal Error Detected (6.2, Figure
    // 6-2); none once read clear with no Unsupported Request since
    // Status after a poisoned write: Detected Parity Error (7.5.1.2)
    if (k == 0x01 && poison_status)
    {
        Check(R_ERR_STATUS, (v >> 31) & 1, "Status 0x%04x after a poisoned request: Detected Parity Error 0", v >> 16);
        poison_status = false;
    }

    int pc = PcieCapDw();
    if (pc >= 0 && k == pc + 2)
    {
        // Device Status by Figure 6-2 (6.2.5), no AER. An Unsupported Request:
        // UR Detected and Non-Fatal Error Detected; but a non-posted one's with
        // Role-Based Error Reporting is Advisory (6.2.3.2.4.1): no severity bit,
        // UR Detected set or clear accepted (7.8.5 has it, the figure ends
        // before it). A
        // poisoned posted request: Non-Fatal, or with RBER none if Advisory
        // (6.2.3.2.4.3). A Correctable error (Bad TLP, Bad DLLP, Replay Timer
        // Timeout: 6.2.7): Correctable Error Detected. Only once Device
        // Capabilities was read (RBER known)
        bool     known  = cfg_seen[pc + 1];
        bool     rber   = known && ((cfg_first[pc + 1] >> 15) & 1);
        bool     nf_req = (ur_since & UR_P) || (ur_since && !rber) || (poison_dev && !rber);
        bool     nf_may = nf_req || poison_dev || poison_np;   // one report per TLP only recommended (6.2.3.2.3)
        uint32_t bits   = ((ur_since & UR_P) || (ur_since && !rber) ? 0x00080000 : 0) | (nf_req ? 0x00020000 : 0) |
                          (corr_since ? 0x00010000 : 0);
        uint32_t clr    = 0x00040000 | (nf_may ? 0 : 0x00020000);   // Correctable may have causes the RC cannot see
        if (known && (ur_since || poison_dev || corr_since))
        {
            Check(R_ERR_STATUS, (v & bits) == bits && !(v & clr),
                  "Device Status 0x%04x after %s: 0x%04x expected set, 0x%04x clear", v >> 16,
                  ur_since ? "an Unsupported Request" : poison_dev ? "a poisoned request" : "a Correctable error",
                  bits >> 16, clr >> 16);
            corr_since = false;
            poison_np  = false;
        }
        if (!ur_since && ur_clear)
        {
            Check(R_ERR_STATUS, !(v & 0x00080000), "Device Status 0x%04x: UR Detected set, no Unsupported Request since it read 0",
                  v >> 16);
        }
        ur_since   = 0;
        ur_clear   = !(v & 0x00080000);
        poison_dev = false;                     // consumed, also with RBER (nothing expected then)
    }

    if (cfg_wr_pend[k])
    {
        uint32_t bem = 0;
        for (int b = 0; b < 4; b++)
        {
            bem |= ((cfg_wr_be[k] >> b) & 1) ? 0xffu << (8 * b) : 0;
        }
        uint32_t ro  = CfgRoMask(k) & bem;
        uint32_t w1c = CfgW1cMask(k) & bem & cfg_wr_data[k];
        if (ro && cfg_before_ok[k])
        {
            Check(R_CFG_RO, !((v ^ cfg_before[k]) & ro), "DW %03xh: 0x%08x after writing 0x%08x, 0x%08x before (read-only 0x%08x)",
                  k * 4, v, cfg_wr_data[k], cfg_before[k], ro);
        }
        w1c &= cfg_before_ok[k] ? cfg_before[k] : 0;  // only bits that were 1 before: else a 0 proves nothing
        uint32_t keep = CfgW1cMask(k) & bem & ~cfg_wr_data[k] & (cfg_before_ok[k] ? cfg_before[k] : 0);
        if (keep)
        {
            Check(R_CFG_RW1C, (v & keep) == keep, "DW %03xh: 0x%08x after writing 0x%08x: RW1C bits written 0 must stay 1 (0x%08x)",
                  k * 4, v, cfg_wr_data[k], keep);
        }
        if (w1c)
        {
            Check(R_CFG_RW1C, !(v & w1c), "DW %03xh: 0x%08x after writing 0x%08x (RW1C 0x%08x)", k * 4, v, cfg_wr_data[k], w1c);
        }
        if (k >= 4 && k <= 9 && cfg_wr_data[k] == 0xffffffff && bem == 0xffffffff)
        {
            CfgBarSize(k, v);
        }
        cfg_wr_pend[k] = false;
    }

    if (!cfg_seen[k])
    {
        cfg_first[k]       = v;
        cfg_first_speed[k] = GetLinkSpeed(chk_node);
        cfg_seen[k]        = true;

        // Header fields, on the first read
        if (k == 0x00)
        {
            Check(R_CFG_ID, (v & 0xffff) != 0xffff && (v & 0xffff) != 0, "Vendor ID 0x%04x", v & 0xffff);
        }
        else if (k == 0x01)
        {
            Check(R_CFG_HEADER, (v & 0x0010f800) == 0x00100000 && !(v & 0x00070000),
                  "Command/Status 0x%08x: Capabilities List 0, or reserved bits set", v);
        }
        else if (k == 0x03)
        {
            Check(R_CFG_HEADER, !(v & 0x007fff00), "DW 00Ch 0x%08x: Header Type layout or Latency Timer not 0", v);
        }
        else if (k == 0x0d)
        {
            Check(R_CFG_HEADER, (v & 0xffffff03) == 0 && (v & 0xff) >= 0x40, "Capabilities Pointer DW 0x%08x", v);
        }
    }
    cfg_val[k] = v;
}

// At the end: the chains and capabilities, from the first reads; a check
// counts only once every DW it needs was read
static void CfgReport(void)
{
    // The capability chain
    if (cfg_seen[0x0d])
    {
        uint32_t ptr = cfg_first[0x0d] & 0xff;
        bool     pm = false, pcie = false, ok = true, ended = false, unread = false;
        for (int n = 0; n <= 48; n++)           // 48 capabilities fill 40h-FFh; the 49th pointer must be 0
        {
            if (ptr == 0)
            {
                ended = true;
                break;
            }
            if (ptr < 0x40 || (ptr & 3))
            {
                ok = false;
                break;
            }
            if (!cfg_seen[ptr / 4])
            {
                unread = true;
                break;
            }
            pm   |= (cfg_first[ptr / 4] & 0xff) == 0x01;
            pcie |= (cfg_first[ptr / 4] & 0xff) == 0x10;
            ptr   = (cfg_first[ptr / 4] >> 8) & 0xff;
        }
        if (!unread)
        {
            Check(R_CFG_CAPS, ok && ended && pm && pcie, "capability chain: %s at %02xh, PM %s, PCIe %s",
                  !ok ? "bad pointer" : ended ? "ends" : "loops", ptr, pm ? "found" : "missing", pcie ? "found" : "missing");
        }
    }

    // The PCIe Capability: the Capabilities register, Device and Link
    // Capabilities, Link Status
    int p = PcieCapDw();
    if (p >= 0 && cfg_seen[p + 1])
    {
        uint32_t caps = cfg_first[p] >> 16, ver = caps & 0xf, type = (caps >> 4) & 0xf;
        // Role-Based Error Reporting: set by every function of PCIe 1.1 on (7.8.3)
        bool gen2 = cfg_seen[p + 3] && (cfg_first[p + 3] & 0xf) >= 2;   // 2.1 hardwires 2h; 1 allowed for a 1.x (2.5 GT/s) device
        Check(R_CFG_PCIE_CAP, (ver == 2 || (ver == 1 && !gen2)) && (type == 0 || type == 1) && !(caps & 0x100) &&
                              (cfg_first[p + 1] & 7) <= 5 && ((cfg_first[p + 1] >> 15) & 1),
              "PCIe Capabilities 0x%04x (version %u, type %u), Device Capabilities 0x%08x (Role-Based Error Reporting %u)",
              caps, ver, type, cfg_first[p + 1], (cfg_first[p + 1] >> 15) & 1);
    }
    if (p >= 0 && cfg_seen[p + 3] && cfg_seen[p + 4])
    {
        uint32_t lcap = cfg_first[p + 3], lsta = cfg_first[p + 4] >> 16;
        uint32_t max_speed = lcap & 0xf, max_width = (lcap >> 4) & 0x3f;
        uint32_t speed = lsta & 0xf, width = (lsta >> 4) & 0x3f;
        bool     width_ok = max_width == 1 || max_width == 2 || max_width == 4 || max_width == 8 || max_width == 12 ||
                            max_width == 16 || max_width == 32;
        Check(R_CFG_LINK, (max_speed == 1 || max_speed == 2) && width_ok && speed == cfg_first_speed[p + 4] &&
                          speed <= max_speed && (int)width == chk_width && width <= max_width,
              "Link Capabilities 0x%08x (Gen%u x%u), Link Status 0x%04x (Gen%u x%u), the link Gen%u x%u", lcap, max_speed,
              max_width, lsta, speed, width, cfg_first_speed[p + 4], chk_width);
    }

    // The extended capability chain from 100h
    if (cfg_seen[0x40])
    {
        uint32_t off = 0x100;
        bool     ok = false, unread = false;
        for (int n = 0; n < 64; n++)            // more than 64: a loop
        {
            if (!cfg_seen[off / 4])
            {
                unread = true;
                break;
            }
            uint32_t h    = cfg_first[off / 4];
            uint32_t next = h >> 20;
            if (h == 0 && off == 0x100)
            {
                ok = true;                      // no extended capabilities
                break;
            }
            if ((h & 0xffff) == 0 || h == 0xffffffff || (next && (next < 0x100 || (next & 3))))
            {
                break;
            }
            if (!next)
            {
                ok = true;
                break;
            }
            off = next;
        }
        if (!unread)
        {
            Check(R_CFG_EXT_CAPS, ok, "extended capability at %03xh: 0x%08x", off, cfg_first[off / 4]);
        }
    }
}

// A memory read's data, 'n' bytes from 'skip' into the payload, against what
// the RC wrote: before the read, or after it (a write may pass a read)
static void CplOrder(const PktData_t *d, const Req &r, const uint32_t n, const uint32_t skip, const uint32_t tag)
{
    const PktData_t *p     = &d[TLP_DATA_OFFSETCPL];
    uint32_t         off   = r.next - r.start;
    bool             known = false;
    for (uint32_t i = 0; i < n && off + i < r.exp.size(); i++)
    {
        int  e    = r.exp[off + i], v = p[skip + i] & 0xff;
        bool late = false;
        for (uint8_t a : r.alt[off + i])
        {
            late |= v == a;
        }
        if (e < 0)
        {
            continue;
        }
        known = true;
        if (v != e && !late)
        {
            Check(R_ORDER, false, "tag %u, address 0x%llx: 0x%02x, written 0x%02x", tag,
                  (unsigned long long)(r.start + off + i), v, e);
            return;
        }
        // A poisoned write's byte: the read shows which value is there now
        auto q = maybe.find(r.start + off + i);
        if (q != maybe.end())
        {
            mem[q->first] = v;
            maybe.erase(q);
        }
    }
    if (known)
    {
        Check(R_ORDER, true, "");
    }
}

// An endpoint Completion, against the RC's request with its tag
static void RxCpl(const PktData_t *d)
{
    bool     data   = H(d, 0) & TL_TYPE_WRITE;
    bool     locked = (H(d, 0) & 0x1f) == 0x0b;
    uint32_t cid    = (H(d, 4) << 8) | H(d, 5);
    uint32_t status = H(d, 6) >> 5;
    uint32_t bcm    = (H(d, 6) >> 4) & 1;
    uint32_t bc     = ((H(d, 6) & 0xf) << 8) | H(d, 7);
    uint32_t rid    = (H(d, 8) << 8) | H(d, 9);
    uint32_t tag    = H(d, 10);
    uint32_t la     = H(d, 11) & 0x7f;
    uint32_t len    = Len(d);
    Req     &r      = req[tag];

    bc = bc ? bc : 4096;

    Check(R_CPL_MATCH, r.live && rid == r.rid, "Completion tag %u, Requester ID 0x%04x: %s 0x%04x", tag, rid,
          r.live ? "the request's" : "no request outstanding, last", r.rid);
    if (!r.live)
    {
        return;
    }

    // Completer ID: the bus/device captured from the last Type 0 CfgWr, or
    // from the one this completes
    int bus = cid >> 8, dev = (cid >> 3) & 0x1f;
    // (a UR for a function the device lacks: its function number is not checked)
    bool fn_ok = (cid & 7) == r.fn || (status == CPL_UNSUPPORTED && r.fn != 0);
    Check(R_CPL_ID, fn_ok && (ep_bus < 0 || (bus == ep_bus && dev == ep_dev) ||
                                          (bus == r.cap_bus && dev == r.cap_dev)),
          "tag %u, Completer ID %02x:%02x.%x, captured %02x:%02x, function %u addressed", tag, bus, dev, cid & 7,
          ep_bus & 0xff, ep_dev & 0xff, r.fn);

    bool        sc  = status == CPL_SUCCESS;
    bool        rd  = r.kind != REQ_IO_CFG_WR;
    const char *bad = NULL;
    if (status != CPL_SUCCESS && status != CPL_UNSUPPORTED && status != CPL_CRS && status != CPL_ABORT)
    {
        bad = "reserved status";
    }
    else if (status == CPL_CRS && !r.cfg)
    {
        bad = "CRS for a request other than Configuration";
    }
    else if (data && !sc)
    {
        bad = "data with a status other than SC";
    }
    else if (sc && data != rd)
    {
        bad = rd ? "SC without data for a read" : "data for a write";
    }
    else if (locked && r.kind != REQ_MEM_RDLK)
    {
        bad = "CplLk for a request other than MRdLk";
    }
    else if (r.kind == REQ_MEM_RDLK && !locked)
    {
        bad = "MRdLk completed without CplLk or CplDLk";
    }
    Check(R_CPL_STATUS, !bad, "tag %u, status %u: %s", tag, status, bad ? bad : "");
    if (r.cfg && status == CPL_CRS)
    {
        Check(R_CRS, !cfg_sc_seen, "tag %u: CRS after a Configuration Request already completed successfully", tag);
    }
    if (status == CPL_UNSUPPORTED)
    {
        ur_since |= UR_NP;
    }

    Check(R_CPL_TC_ATTR, ((H(d, 1) >> 4) & 7) == r.tc && ((H(d, 2) >> 4) & 3) == r.attr,
          "tag %u, TC %u Attr %u, the request's TC %u Attr %u", tag, (H(d, 1) >> 4) & 7, (H(d, 2) >> 4) & 3, r.tc,
          r.attr);

    // Byte Count and Lower Address: memory reads count down the bytes still
    // to come, a split ends on the RCB; IO and Cfg: 4 and 0
    bool     last   = true;
    bool     ok     = bcm == 0 && !(H(d, 11) & 0x80);
    uint32_t exp_bc = 4, exp_la = 0;
    if (sc && (r.kind == REQ_MEM_RD || r.kind == REQ_MEM_RDLK))
    {
        uint32_t delivered = len * 4 - (la & 3);
        exp_bc             = r.remaining;
        exp_la             = r.addr & 0x7f;
        ok                &= bc == exp_bc && la == exp_la;
        if (data && r.kind == REQ_MEM_RD)
        {
            CplOrder(d, r, (delivered < r.remaining) ? delivered : r.remaining, la & 3, tag);
        }
        if (delivered >= r.remaining)
        {
            ok         &= delivered - r.remaining < 4;
            r.remaining = 0;
        }
        else
        {
            ok          &= ((r.addr + delivered) % CHK_RCB_BYTES) == 0;
            r.remaining -= delivered;
            r.addr      += delivered;
            r.next      += delivered;
            last         = false;
        }
    }
    else if (r.kind == REQ_IO_CFG_RD || r.kind == REQ_IO_CFG_WR)
    {
        ok &= bc == 4 && la == 0 && (!data || len == 1);
    }
    else
    {
        exp_bc = bc;                            // a memory read's error status: not checked
        exp_la = la;
    }
    Check(R_CPL_FIELDS, ok, "tag %u: Byte Count %u (expected %u), Lower Address 0x%02x (0x%02x), Length %u DW, BCM %u",
          tag, bc, exp_bc, la, exp_la, data ? len : 0, bcm);

    if (last)
    {
        if (sc && r.cap_bus >= 0)
        {
            ep_bus = r.cap_bus;
            ep_dev = r.cap_dev;
        }
        r.live = false;
        Check(R_CPL_ALL, true, "");
        if (r.cfg && sc)
        {
            cfg_sc_seen = true;
        }
        if (r.expect_ur || status == CPL_UNSUPPORTED)
        {
            Check(R_UR, r.expect_ur == (status == CPL_UNSUPPORTED), "tag %u: %s request completed with status %u", tag,
                  r.expect_ur ? "an unsupported" : "a supported", status);
        }

        // Configuration space
        if (r.cfg && r.fn != 0 && cfg_seen[3] && !(cfg_first[3] & 0x00800000))
        {
            Check(R_CFG_FN, status == CPL_UNSUPPORTED, "Cfg to function %u of a single-function device: status %u", r.fn,
                  status);
        }
        if (r.cfg0 && sc)
        {
            if (r.kind == REQ_IO_CFG_WR)
            {
                CfgWritten(r);
            }
            else if (data)
            {
                const PktData_t *p = &d[TLP_DATA_OFFSETCPL];
                CfgRead(r.reg, (p[0] & 0xff) | ((p[1] & 0xff) << 8) | ((p[2] & 0xff) << 16) | ((uint32_t)(p[3] & 0xff) << 24));
            }
        }
    }
}

// ---- Endpoint TLPs --------------------------------------------------------

static void RxTlp(const pPkt_t pkt, const int edb)
{
    const PktData_t *d = pkt->data;

    const char *bad = Framing(d, true);
    Check(R_FRAMING, !bad, "TLP sequence number %d: %s", pkt->seq, bad ? bad : "");
    if (bad)
    {
        return;
    }

    // LCRC: good, or inverted on a TLP nullified with EDB
    bool     has_data = d[TLP_TYPE_BYTE_OFFSET] & TL_TYPE_WRITE;
    uint32_t lcrc_at  = 15 + 4 * ((has_data ? Len(d) : 0) + TLP_HAS_DIGEST(d) + TLP_HDR_4DW(d));
    std::vector<PktData_t> c = Copy(d);
    if (c.size() < lcrc_at + 5)
    {
        Check(R_LCRC, false, "TLP of %u symbols, LCRC expected at %u", (unsigned)c.size() - 1, lcrc_at);
        return;
    }
    CalcLcrc(c.data());
    bool good = true, inverted = true;
    for (int i = 0; i < 4; i++)
    {
        good     &= c[lcrc_at + i] == d[lcrc_at + i];
        inverted &= (~c[lcrc_at + i] & 0xff) == d[lcrc_at + i];
    }
    Check(R_LCRC, edb ? inverted : good, "TLP sequence number %d, %s", pkt->seq, edb ? "EDB without the inverted LCRC" : "bad LCRC");
    if (edb || !good)
    {
        return;                                 // not a TLP the link layer takes
    }

    Check(R_TLP_EARLY, ep_init2_seen || rc_init2_sent, "TLP sequence number %d before FC_INIT2 could end", pkt->seq);

    // Sequence number: the next new one, or a replay
    int seq = pkt->seq & 0xfff;
    if (seq == ep_next_seq)
    {
        Check(R_SEQ, true, "");
        // A replay goes to the newest TLP before any new one, but for those Acked
        // meanwhile (3.5.2.1): left unsent and unAcked, a TLP was skipped
        if (ep_last_rpl >= 0)
        {
            int  last  = (seq - 1) & 0xfff;
            bool acked = NotBehind(ep_acked, last, 4096) ||
                         L0Now() - ep_ack_ns <= CHK_LINK_RT_NS + CHK_TLP_MAX_SYM * ((GetLinkSpeed(chk_node) == 2) ? 2 : 4);
            Check(R_REPLAY_ORDER, acked, "TLP %d new while the replay left TLPs %d to %d unsent and unAcked", seq,
                  (ep_last_rpl + 1) & 0xfff, last);
        }
        ep_last_rpl = -1;                       // a replay after this one is a new run
        // A TLP already on its way when the Nak came may finish first (3.5.2.1)
        uint64_t sym = (GetLinkSpeed(chk_node) == 2) ? 2 : 4;
        if (rc_nak_owed && L0Now() - rc_nak_ns > CHK_LINK_RT_NS + CHK_TLP_MAX_SYM * sym)
        {
            Check(R_REPLAY_ORDER, false, "TLP %d new, before the replay the RC's Nak asked for (from %d)", seq,
                  (ep_acked + 1) & 0xfff);
            rc_nak_owed = false;
        }
        ep_next_seq  = (seq + 1) & 0xfff;
        if (L0Now() - rc_nak_ns > CHK_LINK_RT_NS + CHK_TLP_MAX_SYM * sym)
        {
            rc_nak_since = false;               // (a TLP already on its way when the Nak came keeps its excuse)
        }
        ep_tx_ns[seq] = L0Now();

        // Transaction Layer rules, once per TLP
        RxTlpHeader(d, c.size() - 1, lcrc_at);
        if ((H(d, 0) & 0x1e) == 0x0a)
        {
            RxCpl(d);
        }

        // A new TLP consumes the RC's credits
        int t = TlpFcType(d);
        ep_cons_hdr[t]  += 1;
        ep_cons_data[t] += TlpDataCredits(d);
        if (rc_fc_valid[t])
        {
            bool hdr_ok  = rc_lim_hdr[t]  == FC_INFINITE_CREDITS || ((rc_lim_hdr[t]  - ep_cons_hdr[t])  & 0xff)  <= 128;
            bool data_ok = rc_lim_data[t] == FC_INFINITE_CREDITS || ((rc_lim_data[t] - ep_cons_data[t]) & 0xfff) <= 2048;
            Check(R_EP_CREDITS, hdr_ok && data_ok, "TLP %d of type %d: consumed %u/%u, the RC's limit %u/%u",
                  seq, t, ep_cons_hdr[t] & 0xff, ep_cons_data[t] & 0xfff, rc_lim_hdr[t], rc_lim_data[t]);
            if (t == FC_POST && hdr_ok && rc_lim_hdr[t] != FC_INFINITE_CREDITS)
            {
                uint32_t left   = (rc_lim_hdr[t] - ep_cons_hdr[t]) & 0xff;   // for the report: starved to 0?
                ep_post_hdr_min = (left < ep_post_hdr_min) ? left : ep_post_hdr_min;
            }
        }
    }
    else if (NotBehind(ep_next_seq - 1, seq, 4096))
    {
        // A replay (3.5.2.1) starts with the oldest unAcked TLP, and the timer
        // ran from its transmission or the last Ack that advanced, whichever
        // came later; the TLPs after it in the same replay go with it
        // The same replay goes on in order, and may skip TLPs Acked meanwhile;
        // a new one starts at the oldest unAcked TLP, or at one whose Ack
        // was still on its way (3.5.2.1)
        int      next  = (ep_last_rpl + 1) & 0xfff;
        uint64_t sym   = (GetLinkSpeed(chk_node) == 2) ? 2 : 4;
        bool     soon  = ep_last_rpl >= 0 && L0Now() - ep_tx_ns[ep_last_rpl] <= CHK_LINK_RT_NS + 2 * CHK_TLP_MAX_SYM * sym;
        bool     go_on = soon && (seq == next || (NotBehind(seq, next, 4096) && NotBehind((ep_acked + 1) & 0xfff, seq, 4096)));
        bool     fresh = L0Now() - ep_ack_ns <= CHK_LINK_RT_NS + CHK_TLP_MAX_SYM * sym;
        int      from  = fresh ? (ep_acked_prev + 1) & 0xfff : (ep_acked + 1) & 0xfff;
        if (!go_on)
        {
            Check(R_REPLAY_ORDER, NotBehind(seq, from, 4096) && NotBehind((ep_acked + 1) & 0xfff, seq, 4096),
                  "a replay from TLP %d, the oldest unAcked %d", seq, (ep_acked + 1) & 0xfff);
            ep_rpl_runs++;
            rc_nak_owed    = false;
            uint64_t start = (ep_tx_ns[seq] > ep_ack_ns) ? ep_tx_ns[seq] : ep_ack_ns;
            uint64_t since = L0Now() - start;
            uint64_t timer = (GetLinkSpeed(chk_node) == 2) ? CHK_REPLAY_GEN2_SYM * 2 : CHK_REPLAY_GEN1_SYM * 4;
            // not before the timer (-0%), and not after twice it (+100%), with
            // the link's round trip and one TLP as slack
            Check(R_REPLAY, rc_nak_since || (since >= timer && since <= 2 * timer + CHK_REPLAY_SLACK_NS),
                  "TLP %d sent again %llu ns after the replay timer started, no Nak from the RC, replay timer %llu ns",
                  seq, (unsigned long long)since, (unsigned long long)timer);
            corr_since  |= !rc_nak_since;        // the endpoint's Replay Timer Timeout: Correctable
            rc_nak_since = false;               // a Nak excuses one replay
        }
        ep_last_rpl   = (seq == ((ep_next_seq - 1) & 0xfff)) ? -1 : seq;   // the newest TLP ends the replay
        ep_tx_ns[seq] = L0Now();
        ep_rpl_tlps++;
    }
    else
    {
        Check(R_SEQ, false, "TLP %d, expected %d", seq, ep_next_seq);
        ep_next_seq = (seq + 1) & 0xfff;
    }
}

// ---- RC packets -----------------------------------------------------------

static void TxPkt(const pPkt_t pkt)
{
    const PktData_t *d = pkt->data;

    // Self-check: the model's own framing
    const char *bad = Framing(d, pkt->seq != DLLP_SEQ_ID);
    if (bad)
    {
        VPrint("**** ERROR: RC model self-check at %.3f us: its %s %d: %s\n", Now() / 1000.0,
               pkt->seq == DLLP_SEQ_ID ? "DLLP type" : "TLP", pkt->seq == DLLP_SEQ_ID ? (int)(d[1] & 0xff) : pkt->seq, bad);
        model_errors++;
    }
    uint32_t t = H(d, 0) & 0x1f, n = GET_TLP_LENGTH(d) ? GET_TLP_LENGTH(d) : 1024;
    if (pkt->seq != DLLP_SEQ_ID && (t == 0x00 || t == 0x01) &&
        (((H(d, 0) & TL_TYPE_WRITE) ? n * 4 > CHK_MPS_BYTES : n * 4 > CHK_MRRS_BYTES) ||
         (GET_TLP_ADDRESS(d) & 0xffc) + n * 4 > 4096))
    {
        VPrint("**** ERROR: RC model self-check at %.3f us: its TLP %d, Fmt/Type 0x%02x, %u DW at 0x%llx: over MPS/MRRS or across 4 KB\n",
               Now() / 1000.0, pkt->seq, H(d, 0), n, (unsigned long long)GET_TLP_ADDRESS(d));
        model_errors++;
    }

    if (pkt->seq == DLLP_SEQ_ID && (rc_fault_arm & FAULT_TX_BAD_DLLP))
    {
        rc_fault_arm &= ~FAULT_TX_BAD_DLLP;     // sent with a bad CRC: the endpoint drops it
        corr_since    = true;                   // its Bad DLLP
        return;
    }
    if (pkt->seq == DLLP_SEQ_ID)
    {
        int type = d[1] & 0xff;
        int base = ((type & 0x40) || (type & 0x80)) ? (type & 0xf8) : type;
        if (base == DL_ACK || base == DL_NAK)
        {
            // The replay timer's late bound: an unAcked TLP waits at most
            // twice the timer for its replay (3.5.2.1, +100%)
            int      a     = ((d[3] & 0xf) << 8) | (d[4] & 0xff);
            int      old   = (ep_acked + 1) & 0xfff;
            uint64_t timer = (GetLinkSpeed(chk_node) == 2) ? CHK_REPLAY_GEN2_SYM * 2 : CHK_REPLAY_GEN1_SYM * 4;
            if (old != ep_next_seq && NotBehind((ep_next_seq - 1) & 0xfff, old, 4096))
            {
                uint64_t start = (ep_tx_ns[old] > ep_ack_ns) ? ep_tx_ns[old] : ep_ack_ns;
                uint64_t wait  = L0Now() - start;
                Check(R_REPLAY, wait <= 2 * timer + CHK_REPLAY_SLACK_NS, "TLP %d unAcked for %llu ns, not sent again, replay timer %llu ns",
                      old, (unsigned long long)wait, (unsigned long long)timer);
            }
            if (base == DL_NAK)
            {
                rc_naks++;
                rc_nak_ns    = L0Now();
                rc_nak_since = true;
                rc_nak_owed  = a != ((ep_next_seq - 1) & 0xfff);   // TLPs after it to send again
            }
            // Ack and Nak both acknowledge up to a
            if (a != ep_acked && NotBehind(a, ep_acked, 4096))
            {
                ep_acked_prev = ep_acked;
                ep_acked      = a;
                ep_ack_ns     = L0Now();
            }
        }
        else if (base == DL_INITFC2_P || base == DL_INITFC2_NP || base == DL_INITFC2_CPL)
        {
            rc_init2_sent = true;
        }
        if (base == DL_INITFC1_P || base == DL_INITFC1_NP || base == DL_INITFC1_CPL ||
                 base == DL_UPDATEFC_P || base == DL_UPDATEFC_NP || base == DL_UPDATEFC_CPL)
        {
            int      t = (base >> 4) & 3;
            uint32_t hdr, data;
            FcFields(d, &hdr, &data);
            if (!rc_fc_valid[t] || base >= DL_UPDATEFC_P)
            {
                rc_lim_hdr[t]  = hdr;
                rc_lim_data[t] = data;
                rc_fc_valid[t] = true;
            }
        }
        return;
    }

    // A TLP: new ones consume the endpoint's credits, and wait for an Ack
    int seq = pkt->seq & 0xfff;

    // The RC's last Completion to one of the endpoint's requests
    if (seq == ((rc_last_seq + 1) & 0xfff) && (H(d, 0) & 0x1f) == 0x0a && ep_bus >= 0 &&
        (uint32_t)((H(d, 8) << 8) | H(d, 9)) == (uint32_t)((ep_bus << 8) | (ep_dev << 3)))
    {
        uint32_t bc = ((H(d, 6) & 0xf) << 8) | H(d, 7);
        ep_np_done += !(H(d, 0) & TL_TYPE_WRITE) || (bc ? bc : 4096) <= Len(d) * 4;
    }
    if (seq == ((rc_last_seq + 1) & 0xfff) && (rc_fault_arm & FAULT_TX_BAD_LCRC))
    {
        rc_bad_seq    = seq;
        rc_fault_arm &= ~FAULT_TX_BAD_LCRC;
        corr_since    = true;                   // the endpoint's Bad TLP
    }
    else if (seq == rc_last_seq && (rc_fault_arm & FAULT_TX_DUP))
    {
        rc_dup_seq    = seq;
        rc_dup_ns     = L0Now();
        rc_dup_gen    = GetLinkSpeed(chk_node);
        rc_fault_arm &= ~FAULT_TX_DUP;
    }
    if (seq == ((rc_last_seq + 1) & 0xfff))
    {
        int t = TlpFcType(d);
        rc_cons_hdr[t]  += 1;
        rc_cons_data[t] += TlpDataCredits(d);
        rc_last_seq      = seq;
        TxRequest(d);
    }
    // Timed from its last transmission; a replay of a TLP Acked meanwhile
    // (a repeated Nak) needs no Ack
    if (seq != rc_acked && NotBehind(seq, rc_acked, 4096))
    {
        rc_tx_ns[seq]   = L0Now();
        rc_tx_gen[seq]  = GetLinkSpeed(chk_node);
        rc_pending[seq] = true;
    }
}

// ---- Monitor --------------------------------------------------------------

// ---- Physical Layer: ordered sets (SetOsMonitor()) and the LTSSM --------------

static uint64_t pl_l0_ns       = 0;         // GetLtssmL0Ns() last seen: a new value, back in L0
static int      pl_speed       = 0;         // the link speed in the last L0
static bool     pl_ep_training = false;     // the endpoint sent TS1/TS2 since the link was last in L0
static bool     pl_rec_spdchg  = false;     // ... TS2 with speed_change, and the rates they advertised
static int      pl_rec_rates   = 0;
static int      pl_train_rates = -1;        // the rates advertised in Polling and Configuration
static int      pl_cfg_link[MAX_LINK_WIDTH], pl_cfg_lane[MAX_LINK_WIDTH];   // set in Configuration
static bool     pl_cfg_set     = false;     // the link has been in L0: Link and Lane Numbers set
static uint64_t pl_skp_base    = 0;         // the last SKP in this L0 (or L0 entry), when
static unsigned pl_skp_n       = 0;         // SKP Ordered Sets in L0, and L0 time in Symbol Times (L0Now()):
static uint64_t pl_l0_sym      = 0;         // the periods before this one, and this one's start and Symbol Time
static uint64_t pl_l0_from     = 0, pl_l0_sym_ns = 4;
static unsigned pl_l0_periods  = 0;
static uint64_t pl_skp_min     = ~0ull, pl_skp_max = 0;
static bool     pl_skp_first   = false;     // the next SKP is the first of this L0
static int      pl_state       = -1;        // the RC's LTSSM state at the last callback
static unsigned pl_eios_n      = 0;         // EIOS in the burst open (0: none), the last one's time,
static uint64_t pl_eios_ns     = 0;         // the rate at the first, and the shortest idle after
static int      pl_eios_gen    = 0;
static uint64_t pl_eios_min    = 0;
static bool     pl_eios_spdchg = false;     // TS2s with speed_change since L0 or the last idle, how many
static unsigned pl_eios_sc_n   = 0;
static uint64_t pl_rec_ns      = 0;         // the RC entered Recovery, or the endpoint last left idle,
static bool     pl_rec_ts2     = false;     // and the endpoint sent TS2 since (in RcvrCfg)

static uint64_t PlSym(void)
{
    return (GetLinkSpeed(chk_node) == 2) ? 2 : 4;
}

// Back in L0: the speed change the Recovery made, and a new SKP interval
static void PlTick(void)
{
    uint64_t l0 = GetLtssmL0Ns(chk_node);
    if (l0 == pl_l0_ns || GetLtssmState(chk_node) != CHK_LTSSM_L0)
    {
        return;
    }
    int spd = GetLinkSpeed(chk_node);
    if (pl_cfg_set && spd != pl_speed)
    {
        Check(R_PL_SPEED, spd == 1 || (pl_rec_spdchg && (pl_rec_rates & 4)),
              "Gen%d to Gen%d: the endpoint's TS2s %s speed_change, advertised rates 0x%02x", pl_speed, spd,
              pl_rec_spdchg ? "with" : "without", pl_rec_rates);
    }
    pl_speed       = spd;
    pl_l0_ns       = l0;
    pl_cfg_set     = true;
    pl_ep_training = pl_rec_spdchg = pl_eios_spdchg = false;
    pl_rec_rates   = 0;
    pl_eios_sc_n   = 0;
    pl_skp_base    = l0;
    pl_skp_first   = true;
    if (pl_l0_periods++)
    {
        pl_l0_sym += (L0Now() - pl_l0_from) / pl_l0_sym_ns;
    }
    pl_l0_from   = L0Now();
    pl_l0_sym_ns = PlSym();
}

// The endpoint out of electrical idle (any ordered set or packet): the EIOS
// burst before it, and the idle time
static void PlEiosEnd(uint64_t now)
{
    if (!pl_eios_n)
    {
        return;
    }
    // Idle until its receivers are, then the minimum: from the later EIOS, its own or the RC's
    uint64_t rc   = GetLtssmEiosNs(chk_node);
    uint64_t base = (rc > pl_eios_ns) ? rc : pl_eios_ns;
    Check(R_PL_EIOS, pl_eios_n >= (unsigned)pl_eios_gen, "%u EIOS in a row at Gen%d", pl_eios_n, pl_eios_gen);
    Check(R_PL_EIOS, rc > pl_rec_ns, "out of electrical idle before the RC went idle");
    Check(R_PL_EIOS, now - base >= pl_eios_min, "out of electrical idle %llu ns after the last EIOS (%s), at least %llu",
          (unsigned long long)(now - base), (rc > pl_eios_ns) ? "the RC's" : "its own", (unsigned long long)pl_eios_min);
    pl_eios_n  = 0;
    pl_rec_ns  = now;
    pl_rec_ts2 = false;
}

static void OsMonitor(int type, int lane, pTS_t ts, void *usrptr)
{
    PlTick();
    uint64_t now  = GetLinkTimeNs(chk_node);
    int      st   = GetLtssmState(chk_node);
    bool     train = st == CHK_LTSSM_POLLING || st == CHK_LTSSM_CONFIG;
    if (st != pl_state)
    {
        if (st == CHK_LTSSM_DETECT)
        {
            pl_cfg_set     = false;             // trained again from Detect
            pl_train_rates = -1;
            pl_eios_n      = 0;
        }
        if (train && pl_state != CHK_LTSSM_POLLING && pl_state != CHK_LTSSM_CONFIG)
        {
            pl_skp_base = now;                  // out of electrical idle: the SKP interval runs (4.2.7.1)
        }
        if (st == CHK_LTSSM_RECOVERY)
        {
            pl_rec_ns  = now;
            pl_rec_ts2 = false;
        }
        pl_state = st;
    }
    // An L0 too short for an ordered set from the endpoint: its next Recovery
    // starts after it all the same
    if (GetLtssmL0Ns(chk_node) > pl_rec_ns)
    {
        pl_rec_ns  = GetLtssmL0Ns(chk_node);
        pl_rec_ts2 = false;
    }
    // EIOS: before Configuration, the link is idle anyway (the endpoint's EIOS until its reset)
    if (lane == 0 && type == IDL && pl_cfg_set && st != CHK_LTSSM_DETECT)
    {
        if (pl_eios_n && now - pl_eios_ns <= 5 * PlSym())
        {
            pl_eios_n++;                        // the next in a row (4 Symbol Times)
        }
        else
        {
            PlEiosEnd(now);
            // Recovery.Speed after a speed negotiation (at 2.5 GT/s with 5.0 GT/s on both
            // sides); else at 5.0 GT/s or at a rate changed since L0, only from RcvrLock
            // timed out (TS1 only, 4.2.6.4.1) or from RcvrCfg (its TS2) after the RC's
            // EIOS (4.2.6.4.3)
            int  spd  = GetLinkSpeed(chk_node);
            bool neg  = pl_eios_spdchg && (spd == 2 || (pl_rec_rates & GetLtssmRates(chk_node) & 4));
            bool lock = !pl_rec_ts2 && now - pl_rec_ns >= CHK_RCVRLOCK_NS;
            bool cfg  = pl_rec_ts2 && GetLtssmEiosNs(chk_node) > pl_rec_ns;
            Check(R_PL_EIOS, st == CHK_LTSSM_RECOVERY && (neg || ((spd == 2 || spd != pl_speed) && (lock || cfg))),
                  "EIOS in LTSSM state %d at Gen%d (Gen%d in L0): the endpoint's TS2s %s speed_change, rates 0x%02x, the RC's 0x%02x; "
                  "%s, %llu ns since Recovery or idle, the RC's EIOS %s",
                  st, spd, pl_speed, pl_eios_spdchg ? "with" : "without", pl_rec_rates, GetLtssmRates(chk_node),
                  pl_rec_ts2 ? "RcvrCfg" : "RcvrLock", (unsigned long long)(now - pl_rec_ns),
                  GetLtssmEiosNs(chk_node) > pl_rec_ns ? "seen" : "not seen");
            if (neg && spd == pl_speed)
            {
                Check(R_PL_EIOS, pl_eios_sc_n >= CHK_SC_TS2, "EIOS after %u TS2s with speed_change, at least %d",
                      pl_eios_sc_n, CHK_SC_TS2);
            }
            pl_eios_n      = 1;
            pl_eios_gen    = spd;
            pl_eios_min    = pl_eios_spdchg ? CHK_EIOS_IDLE_NS : CHK_EIOS_FAIL_NS;
            pl_eios_spdchg = false;
            pl_eios_sc_n   = 0;
        }
        pl_eios_ns = now;
    }
    else if (lane == 0 && type != IDL)
    {
        PlEiosEnd(now);
    }
    // SKP Ordered Sets in training (not Recovery: its Speed substate is electrically idle)
    if (train && lane == 0 && (now - pl_skp_base) / PlSym() > CHK_SKP_GAP_SYM)
    {
        Check(R_PL_SKP, false, "no SKP Ordered Set for %llu Symbol Times in LTSSM state %d",
              (unsigned long long)((now - pl_skp_base) / PlSym()), st);
        pl_skp_base = now;                      // once per interval missed
    }
    if (type == SKP && lane == 0 && (st == CHK_LTSSM_L0 || train))
    {
        uint64_t gap = (now - pl_skp_base) / PlSym();
        Check(R_PL_SKP, gap <= CHK_SKP_GAP_SYM, "SKP Ordered Set %llu Symbol Times after the last%s",
              (unsigned long long)gap, pl_skp_first ? " (L0 entry)" : "");
        if (st == CHK_LTSSM_L0)
        {
            if (!pl_skp_first)
            {
                pl_skp_min = (gap < pl_skp_min) ? gap : pl_skp_min;
                pl_skp_max = (gap > pl_skp_max) ? gap : pl_skp_max;
            }
            pl_skp_n++;
            pl_skp_first = false;
        }
        pl_skp_base = now;
    }
    if ((type != TS1_ID && type != TS2_ID) || ts == NULL)
    {
        return;
    }
    pl_ep_training = true;
    if (!train && st != CHK_LTSSM_RECOVERY && st != CHK_LTSSM_L0)
    {
        return;                                 // the RC in Detect: the endpoint may be ahead
    }
    int  rate  = ts->datarate & 0xff, ctl = ts->control & 0xff;
    if (train && pl_train_rates < 0)
    {
        pl_train_rates = rate & 0x06;
    }
    Check(R_PL_TS_FIELDS, (rate & 0x02) && !(rate & 0x39) && !(train && (rate & 0x80)) &&
                          !(train && (rate & 0x06) != pl_train_rates) && (ctl & ~0x08) == 0,
          "TS%d on lane %d in LTSSM state %d: Data Rate 0x%02x, Training Control 0x%02x", type == TS1_ID ? 1 : 2,
          lane, st, rate, ctl);
    if (lane < MAX_LINK_WIDTH)
    {
        if (st == CHK_LTSSM_CONFIG && type == TS2_ID && ts->linknum != CHK_PAD)
        {
            pl_cfg_link[lane] = ts->linknum;
            pl_cfg_lane[lane] = ts->lanenum;
        }
        else if (pl_cfg_set && !train)
        {
            Check(R_PL_TS_FIELDS, ts->linknum == pl_cfg_link[lane] && ts->lanenum == pl_cfg_lane[lane],
                  "TS%d on lane %d after Configuration: Link 0x%x, Lane 0x%x; set 0x%x, 0x%x", type == TS1_ID ? 1 : 2,
                  lane, ts->linknum, ts->lanenum, pl_cfg_link[lane], pl_cfg_lane[lane]);
        }
    }
    pl_rec_ts2 |= type == TS2_ID && lane == 0 && st == CHK_LTSSM_RECOVERY;
    if (type == TS2_ID && (rate & 0x80))
    {
        pl_rec_spdchg  = true;
        pl_rec_rates  |= rate & 0x06;
        pl_eios_spdchg = true;
        pl_eios_sc_n  += (lane == 0);
    }
}

static void Monitor(const pPkt_t pkt, const int rx, const int edb, void *usrptr)
{
    PlTick();
    if (rx)
    {
        PlEiosEnd(GetLinkTimeNs(chk_node));
        // The endpoint is in L0 once it has had 8 idle symbols from the RC in
        // Configuration.Idle or Recovery.Idle and sent 16 from the first one
        // received, while the RC may still owe some of its own (4.2.6.3.6,
        // 4.2.6.4.4). Its packet, here once whole, then comes at the earliest
        // 16 symbol times and a DLLP's 8 (over the lanes) after the RC's first
        uint64_t idle  = GetLtssmIdleNs(chk_node);
        uint64_t sym   = (GetLinkSpeed(chk_node) == 2) ? 2 : 4;
        bool     ep_l0 = idle && GetLinkTimeNs(chk_node) - idle >= (16 + (8 + chk_width - 1) / chk_width) * sym;
        Check(R_PL_L0_ONLY, !pl_ep_training || ep_l0, "%s from the endpoint after its TS1/TS2, before L0 (LTSSM state %d)",
              pkt->seq == DLLP_SEQ_ID ? "DLLP" : "TLP", GetLtssmState(chk_node));
    }
    if (!rx)
    {
        TxPkt(pkt);
    }
    else if (pkt->seq == DLLP_SEQ_ID)
    {
        RxDllp(pkt->data);
    }
    else
    {
        RxTlp(pkt, edb);
    }
}

// ---- API ------------------------------------------------------------------

void ChkInit(const int node)
{
    chk_node = node;
    unsigned lanes;
    VRead(LANESADDR, &lanes, 1, node);
    chk_width = (lanes == 4) ? 4 : 1;
    SetPktMonitor(Monitor, NULL, node);
    SetOsMonitor(OsMonitor, NULL, node);
}

int ChkReport(void)
{
    // TLPs still waiting for their Ack, and the time since each type's last UpdateFC
    uint32_t gen   = GetLinkSpeed(chk_node);
    for (int s = 0; s < 4096; s++)
    {
        if (rc_pending[s] && L0Now() - rc_tx_ns[s] > CHK_ACK_LIMIT_NS(rc_tx_gen[s], (int)gen))
        {
            Check(R_ACK_LAT, false, "TLP %d not Acked after %llu ns", s, (unsigned long long)(L0Now() - rc_tx_ns[s]));
        }
    }
    for (int t = 0; t < FC_NUMTYPES; t++)
    {
        bool finite = ep_init1_hdr[t] != FC_INFINITE_CREDITS || ep_init1_data[t] != FC_INFINITE_CREDITS;
        if (finite && ep_fc_last_ns[t])
        {
            uint64_t gap = L0Now() - ep_fc_last_ns[t];
            Check(R_FC_PERIOD, gap <= CHK_UPDATEFC_NS, "no UpdateFC of type %d for %llu ns, to the end", t, (unsigned long long)gap);
        }
    }

    CfgReport();

    // The RC's faults: each hit a TLP and was answered
    if (rc_fault_arm || rc_bad_seq >= 0 || rc_dup_seq >= 0 || rc_nak_owed)
    {
        Check(R_BAD_TLP, rc_bad_seq < 0 && !(rc_fault_arm & FAULT_TX_BAD_LCRC), "a TLP sent with a bad LCRC never Naked");
        Check(R_DUPLICATE, rc_dup_seq < 0 && !(rc_fault_arm & FAULT_TX_DUP), "a TLP sent twice never Acked");
        Check(R_REPLAY_ORDER, !rc_nak_owed, "the replay after the RC's Nak never came");
    }
    VPrint("===> Checker: the RC sent %u Nak(s); the endpoint sent %u TLP(s) again, in %u replay(s)\n", rc_naks,
           ep_rpl_tlps, ep_rpl_runs);
    if (ep_reqs)
    {
        VPrint("===> Checker: the endpoint sent %u request(s); it had %u posted header credit(s) left at least\n", ep_reqs,
               ep_post_hdr_min);
    }

    // Initial credits, against Max_Payload_Size Supported (Device
    // Capabilities; 128 bytes until read); and every credit returned
    int      p      = PcieCapDw();
    uint32_t mps_cr = ((p >= 0 && cfg_seen[p + 1]) ? 128u << (cfg_first[p + 1] & 7) : 128u) / FC_DATA_CREDIT_BYTES;
    for (int t = 0; t < FC_NUMTYPES; t++)
    {
        if (!ep_init1_valid[t])
        {
            continue;
        }
        uint32_t need     = (t == FC_NONPOST) ? 1 : mps_cr;
        bool     hdr_inf  = ep_init1_hdr[t] == FC_INFINITE_CREDITS;
        bool     data_inf = ep_init1_data[t] == FC_INFINITE_CREDITS;
        Check(R_FC_INIT_MIN, data_inf || ep_init1_data[t] >= need, "InitFC1 type %d %u/%u: data at least %u",
              t, ep_init1_hdr[t], ep_init1_data[t], need);
        if (!hdr_inf || !data_inf)
        {
            uint32_t hdr_left  = (ep_lim_hdr[t] - rc_cons_hdr[t]) & 0xff;
            uint32_t data_left = (ep_lim_data[t] - rc_cons_data[t]) & 0xfff;
            Check(R_FC_RETURN, (hdr_inf || hdr_left == ep_init1_hdr[t]) && (data_inf || data_left == ep_init1_data[t]),
                  "type %d: %u/%u credits available at the end, %u/%u advertised", t, hdr_left, data_left,
                  ep_init1_hdr[t], ep_init1_data[t]);
        }
    }

    for (int tag = 0; tag < 256; tag++)
    {
        if (req[tag].live)
        {
            Check(R_CPL_ALL, false, "request tag %d never completed", tag);
        }
    }

    // SKP Ordered Sets: the last gap, and the average interval (4.2.7.1)
    PlTick();
    if (GetLtssmState(chk_node) == CHK_LTSSM_L0)
    {
        uint64_t gap = (GetLinkTimeNs(chk_node) - pl_skp_base) / PlSym();
        Check(R_PL_SKP, gap <= CHK_SKP_GAP_SYM, "no SKP Ordered Set for %llu Symbol Times at the end", (unsigned long long)gap);
    }
    // ... as many as the interval allows in the L0 time, give or take one per L0 period
    if (pl_l0_periods)
    {
        uint64_t t = pl_l0_sym + (L0Now() - pl_l0_from) / pl_l0_sym_ns;
        Check(R_PL_SKP, pl_skp_n + pl_l0_periods >= t / CHK_SKP_MAX_SYM && pl_skp_n <= t / CHK_SKP_MIN_SYM + pl_l0_periods,
              "%u SKP Ordered Sets in %llu Symbol Times of L0 (%u periods)", pl_skp_n, (unsigned long long)t, pl_l0_periods);
        VPrint("===> Checker: the endpoint sent %u SKP Ordered Sets in %llu Symbol Times of L0: one per %llu (%llu to %llu apart)\n",
               pl_skp_n, (unsigned long long)t, (unsigned long long)(pl_skp_n ? t / pl_skp_n : 0),
               (unsigned long long)(pl_skp_n > 1 ? pl_skp_min : 0), (unsigned long long)pl_skp_max);
    }

    int fails = 0;
    VPrint("\n===> Checker: rule              PCIe 2.1   checks  fails  what\n");
    for (int r = 0; r < R_NUM; r++)
    {
        VPrint("===>   %-18s %-8s %8u %6u  %s\n", rules[r].id, rules[r].sec, rules[r].checks, rules[r].fails, rules[r].what);
        fails += rules[r].fails;
    }
    VPrint("===> Checker: %d rules, %d violations\n", R_NUM, fails);
    if (model_errors)
    {
        VPrint("===> Checker: %d model self-check errors\n", model_errors);
    }

    return fails + model_errors;
}

unsigned ChkFails(const char *id)
{
    for (int r = 0; r < R_NUM; r++)
    {
        if (!strcmp(rules[r].id, id))
        {
            return rules[r].fails;
        }
    }
    return ~0u;
}

bool ChkTagLive(const int tag)
{
    return req[tag & 0xff].live;
}

unsigned ChkTagGen(const int tag)
{
    return req_gen[tag & 0xff];
}

void ChkExpectUr(const int tag)
{
    ur_next[tag & 0xff] = true;
}

void ChkUrPosted(void)
{
    ur_since |= UR_P;
}

void ChkFault(const int fault)
{
    rc_fault_arm |= fault;
}

void ChkEpDma(unsigned *reqs, unsigned *np_left)
{
    *reqs    = ep_reqs;
    *np_left = ep_np - ep_np_done;
}

int ChkOutstanding(void)
{
    int n = 0;
    for (const Req &r : req)
    {
        n += r.live;
    }
    return n;
}

bool ChkCreditsReturned(void)
{
    for (int t = 0; t < FC_NUMTYPES; t++)
    {
        if (!ep_init1_valid[t])
        {
            continue;
        }
        bool hdr_inf  = ep_init1_hdr[t] == FC_INFINITE_CREDITS;
        bool data_inf = ep_init1_data[t] == FC_INFINITE_CREDITS;
        if ((!hdr_inf && ((ep_lim_hdr[t] - rc_cons_hdr[t]) & 0xff) != ep_init1_hdr[t]) ||
            (!data_inf && ((ep_lim_data[t] - rc_cons_data[t]) & 0xfff) != ep_init1_data[t]))
        {
            return false;
        }
    }
    return true;
}
