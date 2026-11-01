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
//   The Artix-7 RC model's checker: every TLP and DLLP on the link, both
//   ways (pcieVHost's packet monitor, SetPktMonitor()), checked against
//   rules of the PCIe Base Specification. Each rule has an ID, a section
//   (PCIe Base 2.1) and counts of checks and violations; ChkReport()
//   prints the table at the end of the run.
//==========================================================================

#ifndef A7RC_CHECK_H
#define A7RC_CHECK_H

// Starts checking the link of the model at 'node'; call after initialisePcie()
void ChkInit(const int node);

// Prints the rule table; returns the number of violations
int  ChkReport(void);

// For the stimulus: a request with 'tag' still waits for its Completion
bool ChkTagLive(const int tag);
unsigned ChkTagGen(const int tag);   // non-posted requests sent with 'tag' so far

// The next request with 'tag' is an Unsupported Request (TL.UR); a posted one
// was sent (the endpoint must still set UR Detected, CFG.ERR-STATUS)
void ChkExpectUr(const int tag);
void ChkUrPosted(void);
void ChkFault(const int fault);                     // the RC injects FAULT_TX_BAD_LCRC or FAULT_TX_DUP (pcie.h) next
void ChkEpDma(unsigned *reqs, unsigned *np_left);   // the endpoint's requests seen, and those not yet completed
int  ChkOutstanding(void);                          // requests still waiting for a Completion
unsigned ChkFails(const char *id);   // a rule's violations so far, by its ID
bool ChkCreditsReturned(void);   // the endpoint has returned every credit the RC's TLPs used

#endif
