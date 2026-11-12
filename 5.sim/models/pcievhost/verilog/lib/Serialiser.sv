//=============================================================
// 
// Copyright (c) 2016 Simon Southwell. All rights reserved.
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

`ifdef VPROC_SV
`include "allheaders.svh"
`endif

`WsTimeScale

// With BitReverse = 1'b0, serialisation/deserialisation is lsb.

module Serialiser (SerClk, BitReverse, ParInVec, SerOut, SerIn, ParOut, RxIdle);

parameter Width = 16;

input                 SerClk;
input                 BitReverse;
input  [Width*10-1:0] ParInVec;
input  [Width-1:0]    SerIn;

output [Width*10-1:0] ParOut;
output [Width-1:0]    SerOut;
output [Width-1:0]    RxIdle;   // openPCIE: per lane, not synced, so the partner is idle

bit    [9:0]          SerialShift   [0:Width-1];
bit    [9:0]          DeserialShift [0:Width-1];
bit    [9:0]          DeserialReg   [0:Width-1];

bit                   Synced        [0:Width-1];   // openPCIE: per lane

int                   SerialCount;
int                   DeserialCount [0:Width-1];
int                   idx;

wire [9:0] ParIn [0:Width-1];

function [9:0] BitRev10;
input [9:0] InVal;
begin
    BitRev10 = {InVal[0], InVal[1], InVal[2], InVal[3], InVal[4],
                InVal[5], InVal[6], InVal[7], InVal[8], InVal[9]};
end
endfunction

genvar i;
generate
    for (i = 0; i < Width; i = i +1)
    begin : io
        assign ParIn [i]           = BitReverse ? BitRev10(ParInVec[i*10+9:i*10]) : ParInVec[i*10+9:i*10];
        assign ParOut[i*10+9:i*10] = Synced[i]  ? (BitReverse ? BitRev10(DeserialReg[i]) : DeserialReg[i]) : 10'b0000000000;
        assign RxIdle[i]           = !Synced[i];
        assign SerOut[i]           = SerialShift[i][0];
    end
endgenerate

// openPCIE: the one model register block left on initial + always: upstream's
// Serialiser has no reset port, and it runs on the bit clock from time 0
initial 
begin
    for (idx = 0; idx < Width; idx = idx + 1)
        SerialShift[idx] = 10'b0000000000;   // openPCIE: the line starts idle, not X
    for (idx = 0; idx < Width; idx = idx + 1)
    begin
        Synced[idx]        = 0;
        DeserialCount[idx] = 0;
    end
    SerialCount   = 9;
end

always @(posedge SerClk) 
begin
    if (SerialCount == 0) 
    begin
        SerialCount <=  9;
        for (idx = 0; idx < Width; idx = idx + 1)
            SerialShift[idx] <= ParIn[idx];
    end
    else 
    begin
        SerialCount <=  SerialCount - 1;
        for (idx = 0; idx < Width; idx = idx + 1)
            SerialShift[idx] <= {1'b0, SerialShift[idx][9:1]};
    end

    // Maintain sync, each lane on its own (openPCIE; upstream: lane 0 for all).
    // openPCIE: lose it with the first idle symbol, ten equal bits (an idle
    // line is constant; no 8b/10b symbol runs past five), so the last symbol
    // before electrical idle stays on ParOut for its symbol time. The same on
    // every simulator. Upstream dropped it at the first X or Z bit, 4-state
    // simulators only: the model missed that symbol (the last IDL of an EIOS),
    // and under Verilator sync was never lost. Re-synced at a comma, the symbol
    // is counted from it; upstream kept the count from before the electrical
    // idle, so after a re-sync every symbol was cut at the old bit offset
    for (idx = 0; idx < Width; idx = idx + 1)
    begin
        DeserialShift[idx] <= {SerIn[idx], DeserialShift[idx][9:1]};

        if (Synced[idx])
        begin
            if ((DeserialShift[idx] == 10'b0000000000 || DeserialShift[idx] == 10'b1111111111) && DeserialCount[idx] == 9)
                Synced[idx] <= 1'b0;
        end
        else if (DeserialShift[idx] == `NCOMMA || DeserialShift[idx] == `PCOMMA)
        begin
            Synced[idx]        <= 1'b1;
            DeserialCount[idx] <= 0;
        end

        if (Synced[idx])
        begin
            DeserialCount[idx] <= (DeserialCount[idx] + 1) % 10;
        end

        if (DeserialCount[idx] == 9 || (!Synced[idx] && (DeserialShift[idx] == `NCOMMA || DeserialShift[idx] == `PCOMMA)))
        begin
            DeserialReg[idx] <= DeserialShift[idx];
        end
    end

end

endmodule

