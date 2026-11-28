<!--
SPDX-FileCopyrightText: 2026 Chili.CHIPS

SPDX-License-Identifier: BSD-3-Clause
-->

<!---
Markdown description for SystemRDL register map.

Don't override. Generated from: openpcie
  - csr_build/csr.rdl
-->

## openpcie address map

- Absolute Address: 0x0
- Base Offset: 0x0
- Size: 0x30003000

|  Offset  |Identifier|Name|
|----------|----------|----|
|0x00000000|   imem   |imem|
|0x30000000|    csr   | csr|

## imem memory

- Absolute Address: 0x0
- Base Offset: 0x0
- Size: 0x2000

<p>CPU Program/Data Memory</p>

No supported members.


## csr address map

- Absolute Address: 0x30000000
- Base Offset: 0x30000000
- Size: 0x3000

<p>openPCIE Root Complex CSR</p>

|Offset|Identifier|     Name     |
|------|----------|--------------|
|0x0000|    tx    |    csr.tx    |
|0x0010|    rx    |    csr.rx    |
|0x0024|  status  |  csr.status  |
|0x1000|rx_payload|csr.rx_payload|
|0x2000|tx_payload|csr.tx_payload|

## tx register file

- Absolute Address: 0x30000000
- Base Offset: 0x0
- Size: 0x10

<p>TLP Transmit CSR (CPU --&gt; PCIe link). The TLPs go in
tx_payload, header first; writing tx.send launches them.</p>

|Offset|Identifier|    Name   |
|------|----------|-----------|
|  0xC |   send   |csr.tx.send|

### send register

- Absolute Address: 0x3000000C
- Base Offset: 0xC
- Size: 0x4

<p>TLP Send -- writing this register STARTS the transfer</p>

|Bits|Identifier|Access|Reset|         Name         |
|----|----------|------|-----|----------------------|
| 7:0|   count  |   w  | 0x0 |csr.tx.send.count[7:0]|

#### count field

<p>The number of TLPs to send. The firmware writes them back
to back into tx_payload from entry 0, header first, then
their number here, LAST: the Tx FSM sends them in order,
each one's length from its DW0 (Fmt and Length).
External: nothing is stored, the write starts the FSM.</p>

## rx register file

- Absolute Address: 0x30000010
- Base Offset: 0x10
- Size: 0x14

<p>TLP Receive CSR (PCIe link --&gt; CPU). Incoming TLPs queue up in
a ring, as they are. rx_payload shows the oldest, the head:
entry i is its DW i. The firmware decodes it, then frees it.</p>

|Offset|Identifier|    Name    |
|------|----------|------------|
| 0x10 |   valid  |csr.rx.valid|

### valid register

- Absolute Address: 0x30000020
- Base Offset: 0x10
- Size: 0x4

<p>Receive Status Register</p>

|Bits|Identifier|Access|Reset|       Name       |
|----|----------|------|-----|------------------|
|  0 |   valid  |  rw  | 0x0 |csr.rx.valid.valid|

#### valid field

<p>Reads 1 while a TLP is waiting in rx_payload. Writing 1
frees it, and the next TLP in the ring takes its place.
External: nothing is stored, the ring is the state.</p>

## status register file

- Absolute Address: 0x30000024
- Base Offset: 0x24
- Size: 0xC

<p>PCIe Link Status CSR</p>

|Offset|Identifier|       Name      |
|------|----------|-----------------|
|  0x0 |    err   |  csr.status.err |
|  0x4 |    phy   |  csr.status.phy |
|  0x8 |  result  |csr.status.result|

### err register

- Absolute Address: 0x30000024
- Base Offset: 0x0
- Size: 0x4

<p>PCIe Error Status Register</p>

|Bits|  Identifier |  Access |Reset|            Name            |
|----|-------------|---------|-----|----------------------------|
| 16 |msg_err_fatal|rw, woclr| 0x0 |csr.status.err.msg_err_fatal|

#### msg_err_fatal field

<p>A fatal-error Message TLP has been received
(cfg_msg_received_err_fatal). Sticky: stays 1 until
software writes 1 to it</p>

### phy register

- Absolute Address: 0x30000028
- Base Offset: 0x4
- Size: 0x4

<p>Tx FSM / PCIe hard macro Status Register</p>

|Bits| Identifier |Access|Reset|              Name              |
|----|------------|------|-----|--------------------------------|
|  0 |   tx_busy  |   r  | 0x0 |     csr.status.phy.tx_busy     |
|13:8|tx_buf_avail|   r  | 0x0 |csr.status.phy.tx_buf_avail[5:0]|

#### tx_busy field

<p>The Tx FSM is sending. Software polls for 0 before it
writes the next TLPs into tx_payload.</p>

#### tx_buf_avail field

<p>Number of free transmit buffers in the PCIe hard macro
(tx_buf_avail). Software waits for a non-zero value.</p>

### result register

- Absolute Address: 0x3000002C
- Base Offset: 0x8
- Size: 0x4

<p>Firmware Result Register</p>

|Bits|Identifier|Access|Reset|             Name             |
|----|----------|------|-----|------------------------------|
|31:0|  marker  |  rw  | 0x0 |csr.status.result.marker[31:0]|

#### marker field

<p>Result marker the firmware writes when it stops, e.g.
0x0000FACE for PASS. Unlike tx.send, writing it launches
no TLP: the value goes to logic only, to the PASS LED and
to an ILA.</p>

## rx_payload memory

- Absolute Address: 0x30001000
- Base Offset: 0x1000
- Size: 0x1000

<p>The head TLP of the RX ring, header first, while rx.valid
reads 1: entry i is its DW i. The window spans the ring:
entries past the head show the TLPs behind it.</p>

No supported members.


## tx_payload memory

- Absolute Address: 0x30002000
- Base Offset: 0x2000
- Size: 0x1000

<p>The TLPs to send, back to back from entry 0, header first:
entry i is DW i of the batch. Writing their number into tx.send
then launches them; each one's Fmt and Length say how many
entries it takes.</p>

No supported members.

