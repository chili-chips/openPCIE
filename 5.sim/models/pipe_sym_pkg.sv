//==========================================================================
// Copyright (C) 2026 Chili.CHIPS
//--------------------------------------------------------------------------
// SPDX-License-Identifier: BSD-3-Clause
//--------------------------------------------------------------------------
// Description:
//   pipe_sym_pkg: the symbol values our pcieVHost models put on a PIPE, in
//   place of including upstream's pciexpress_header.v for them.
//
//   NOSIG_SYM is electrical idle on a PIPE, per symbol: the K flag and 0xFF,
//   which has no 8b/10b code (Base 2.1 4.2.1), so PCIe never sends it. Our
//   pcievhost-speed.patch defines the same value as `NOSIG for upstream's
//   code; pcieVHostPipexN.sv checks that the two agree.
//
//   Master: openPCIE 5.sim/models; openCologne-PCIE has an identical copy.
//==========================================================================

package pipe_sym_pkg;

  localparam logic [8:0] NOSIG_SYM = {1'b1, 8'hFF};   // {K, byte}

endpackage: pipe_sym_pkg

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        Nov 2, 2026
// -----------------------------------------------------------------------------
