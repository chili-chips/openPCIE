# Opensource RC-switched core (bonus)

The Root Complex behind a PCIE switch: the open replacement of the
[AMD-proprietary RC-switched IP stack](../../2.amd-rtl-with-Vivado-build/3.Bonus--RC-switched.amd),
validated with the same test procedure. A bonus deliverable, beyond the original plan.

- ✅ Hardware FSM replaced with an opensource RISC-V based SOC
- ✅ PCIE IP core replaced with opensource RTL
- ✅ Builds with a **fully opensource toolchain**
- ✅ Enumerates an ASM1184e switch and the endpoints behind it
- ✅ SOC uses PeakRDL for CSR generation, from the same `csr.rdl` as RC-direct

---

## Status

| | |
|---|---|
| Target device | `xc7a200tfbg484-3`, Acorn CLE-215P |
| Switch | ASM1184e, 1 upstream + 4 downstream ports |
| Link | PCIE Gen2 x1 (x1 only: the switch ports are x1; x4 is RC-direct only) |
| Vivado build | works |
| Opensource build | works - see [`4.build/hw_build.openXC7`](../../4.build/hw_build.openXC7) |

`PCIE_LANES` ([`link_pkg.sv`](../0.common.opensource/src/pcie/link_pkg.sv)) is always 1 here: `make VARIANT=switched` and the Vivado wrapper refuse x4.

Hardware setup: [1.pcb](../../1.pcb#usecase-2-switched-fpga_rc-to-fpga_ep-gen2-x1). On the RevA backplane the ASM1184e is fed by an undersized LDO (slide 17 of the [presentation](../../1.pcb/0.doc)); RevB uses a DC/DC buck.

---

## What differs from RC-direct

In the RTL: nothing. Both build from the same [`0.common.opensource`](../0.common.opensource) sources. A switch changes nothing at the physical or link layer: the root port trains a plain Gen2 x1 link to the switch's upstream port. What changes is how Configuration TLPs are addressed, which the shared SOC handles and the firmware drives.

### 1. RTL: Type 0 / Type 1 selection (`riscv_pcie_soc.sv`)

Direct, the endpoint sits on bus 1 and every Configuration request is **Type 0**. Behind a switch, requests cross virtual PCI-to-PCI bridges, which forward only **Type 1**.

The AMD design does it in `cgator_pkt_generator.v`:

```verilog
wire [7:0] pkt_bus_num;
wire [4:0] pkt_dev_num;
assign cfg_type_code = (pkt_bus_num > 8'd1) ? 5'b00101 : 5'b00100;
```

There, `pkt_bus_num` / `pkt_dev_num` had to be added: the packet generator built the header itself with a hardcoded target, so the controller now feeds it each transaction's bus and device from the configuration ROM.

Here the SOC bridge **is** the packet generator, and the device number is already in the address dword the firmware wrote. So the bridge reads only the bus number back out of the header and overrides only the Type bit:

```systemverilog
assign tx_dw0     = csr_out.tx.header0.hdr.value;
assign tx_cfg_dw2 = csr_out.tx.header2.hdr.value;
assign pkt_is_cfg = (tx_dw0.tlp_type[4:1] == TLP_TYPE_CFG);

always_comb begin
  tx_dw0_routed = tx_dw0;
  if (pkt_is_cfg) begin
    tx_dw0_routed.tlp_type[0] = (tx_cfg_dw2.bus > 8'd1);
  end
end
```

`tlp_type[4:1]` marks a Configuration request, `tlp_type[0]` selects Type 0 / Type 1 (structs `pcie_tlp_dw0_t`, `pcie_cfg_dw2_t` in `pcie_pkg.sv`). Memory and Completion TLPs pass untouched. RC-direct carries this logic too, but never triggers it (bus 1 is Type 0 either way).

What does differ:

* **The GT lane.** This folder's XDC locks the transceiver to `GTPE2_CHANNEL_X0Y6` (RC-direct: `X0Y5` at x1; at x4, the default, `X0Y6`, `X0Y4`, `X0Y5`, `X0Y7` for lanes 0-3). All other constraints are shared: [lane table](../README.md#common-physical-constraints-xdc).
* **The firmware**, below.

### 2. Firmware: the bring-up sequence

The AMD design runs a 46-entry ROM (`cgator_cfg_rom.data`). Here it is C, [`3.sw/RC-switched/main.c`](../../3.sw/RC-switched/main.c), on the request layer both firmwares share ([`3.sw/common`](../../3.sw/common)). Same steps, same order, same bus map:

```
bus 0 .............. Root Complex
 |
bus 1  dev 0 ....... ASM1184e upstream port      (pri 1, sec 2, sub 6)
 |
bus 2  dev 1 ....... downstream port 1 --> bus 3, window 0x10000000
       dev 3 ....... downstream port 2 --> bus 4, window 0x10100000
       dev 5 ....... downstream port 3 --> bus 5, window 0x10200000
       dev 7 ....... downstream port 4 --> bus 6, window 0x10300000
 |
bus 3..6  dev 0 .... the endpoint cards, BAR0 at the window base
```

Simulate it against a switch model with two endpoints and two empty slots: `make run VARIANT=switched` in [5.sim](../../5.sim/README.md#rc-switched-a-switch-behind-the-link).

1. **Switch upstream port.** Bus numbers (primary 1, secondary 2, subordinate 6), one memory window `0x10000000 - 0x103FFFFF` for everything behind the switch, then Memory Space + Bus Master.
2. **The four downstream ports** on internal bus 2. Each is a virtual bridge with its own secondary bus and its own 1 MB slice of the upstream window.
3. **The endpoints**, device 0 on buses 3..6. Each BAR is sized (write all ones, read back, decode 32/64-bit) and placed, aligned to its size, inside its port's 1 MB window; anywhere else the port drops every memory request to it. So BAR0 lands at the window base. A BAR0 larger than the window is reported, not squeezed in; other BARs that do not fit are parked (64-bit above 4 GB, 32-bit at 0).
4. **Self test.** Memory write and read-back through the switch for each populated slot, then the result marker to `PCIE_RESULT`, as in the direct build.

Comparing the C with the AMD ROM dump:

* Configuration payloads travel **big-endian**: register byte 0 is in bits `[31:24]` of the data dword. Both firmwares swap in `bswap32()`, so values read as the spec prints them (BAR `0x10000000`, not `0x00000010`).
* The ROM writes `0x103F` to a memory-limit field where the firmware writes `0x1030`. Bits `[3:0]` of Memory Limit are read-only, so the switch gets the same value.

Unlike the ROM, the firmware **probes each slot** and skips empty ones, so a partly populated backplane works. Every Completion Status is checked; config reads **and** writes answered with CRS are re-issued up to 100 times. The result goes to `PCIE_RESULT` (`status.result`, `0x3000_0024`), which sends no TLP; PASS lights `led_link_up[3]`, and an ILA can watch every marker ([RTL Debug with ILA](../README.md#rtl-debug-with-ila)):

| Marker | Meaning |
|---|---|
| `0x0000FACE` | every endpoint that was found passed its readback |
| `0x0000DEAD` | an endpoint failed its readback |
| `0xBAD00000` | the switch upstream port did not answer at all |
| `0xBAD00001` | the switch is up, but no endpoint was found behind it |
| `0xBAD00002` | a config request to a device that had answered failed (UR, CA, timeout, or CRS after 100 retries) |
| `0xBAD00003` | an endpoint's BAR0 is not implemented, is I/O, or is larger than its 1 MB port window |

---

## Structure

The RTL and the CSR are shared with RC-direct, in [`0.common.opensource`](../0.common.opensource/README.md); the PCIE stack is described file by file in the [RC-direct README](../2.RC-direct.opensource/README.md#the-pcie-stack-by-layer), and the CSR in [`4.build/README.md`](../../4.build/README.md#csr-hal-compilation). This folder holds only:

```
2.rtl/3.Bonus--RC-switched.opensource/   this folder
  xdc/
    RC-switched.x1g2.AcornCLE-215P.xdc  GT lane: GTPE2_CHANNEL_X0Y6
  RC-switched.opensource.tcl      regenerates the Vivado project from scratch
```

---

## Building

Both flows build the **switched** firmware themselves (`4.build/sw_build`, `make VARIANT=switched`) into their own folder.

### Vivado

```tcl
source RC-switched.opensource.tcl
```

The project lands in `xbuild.Vivado-v2024.2/`.

### Opensource

```sh
cd 4.build/hw_build.openXC7
make VARIANT=switched
```

Output: `4.build/hw_build.openXC7/build_artifacts/top.x1g2.switched.bit`, next to the direct builds. Toolchain: [`4.build/hw_build.openXC7`](../../4.build/hw_build.openXC7).

----
End-of-Document
