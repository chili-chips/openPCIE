//=============================================================
// 
// Copyright (c) 2016 - 2025 Simon Southwell. All rights reserved.
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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pcie.h"

#ifdef __cplusplus
}
#endif

#ifdef OSVVM
#include "OsvvmVUserVPrint.h"
#endif

#ifdef __cplusplus
#define EXTERN extern "C"
#else
#define EXTERN extern
#endif

#ifndef _LTSSM_H_
#define _LTSSM_H_

#define LINK_INIT_NO_CHANGE  (-1)

typedef struct
{
    int ltssm_linknum;
    int ltssm_n_fts;
    int ltssm_ts_ctl;
    int ltssm_detect_quiet_to;
    int ltssm_enable_tests;
    int ltssm_force_tests;
    int ltssm_poll_active_tx_count;
    int ltssm_disable_disp_state;

} ConfigLinkInit_t;

#define INIT_CFG_LINK_STRUCT(_cfg) {                  \
  (_cfg).ltssm_linknum              = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_n_fts                = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_ts_ctl               = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_detect_quiet_to      = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_enable_tests         = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_force_tests          = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_poll_active_tx_count = LINK_INIT_NO_CHANGE; \
  (_cfg).ltssm_disable_disp_state    = LINK_INIT_NO_CHANGE; \
}

// Link initialisation
EXTERN void InitLink             (const int linkwidth,         const int node);
EXTERN void InitLinkGen          (const int linkwidth,         const int gen,   const int node);

// L0 and speed change (openPCIE)
EXTERN void LinkIdle             (const int ticks,             const int node);
EXTERN void ChangeLinkSpeed      (const int gen,               const int node);
EXTERN void RetrainLink          (const int node);
EXTERN void SetLtssmVary         (const int max,               const int node);   // TS counts, idle times: up to max more
EXTERN void SetLaneReversal      (const int on,                const int node);   // may reverse its lanes (4.2.4.9.1)
EXTERN int  GetLinkSpeed         (const int node);
EXTERN uint64_t GetLinkTimeNs    (const int node);
EXTERN uint64_t GetLinkRecoveryNs(const int node);
EXTERN uint32_t GetLinkRecoveryTicks(const int node);
EXTERN int  GetLtssmState        (const int node);
EXTERN uint64_t GetLtssmL0Ns     (const int node);
EXTERN int  GetLtssmRates        (const int node);   // the rates it advertises, TS_DATA_RATE_GEN1/2
EXTERN uint64_t GetLtssmEiosNs   (const int node);   // its last EIOS in Recovery.Speed (GetLinkTimeNs())
EXTERN uint64_t GetLtssmIdleNs   (const int node);   // Configuration.Idle/Recovery.Idle began, else 0

// Link profile (openPCIE): the link partner this node plays
#define LTSSM_PROFILE_EP          0
#define LTSSM_PROFILE_ARTIX7_RC   1
EXTERN void SetLinkProfile       (const int profile,           const int node);
EXTERN int  GetLinkProfile       (const int node);
EXTERN void ConfigLinkInit       (const ConfigLinkInit_t cfg,  const int node);
EXTERN void ConfigurePcieLtssm   (const config_t         type, const int value, const int node);

#endif
