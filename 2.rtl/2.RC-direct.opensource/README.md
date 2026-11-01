# Opensource RC-direct core

The Root Complex for a direct RC-to-EP link: the open replacement of the
[AMD-proprietary RC-direct IP stack](../../2.amd-rtl-with-Vivado-build/2.RC-direct.amd)
it was first tested with. With the [openPCIE backplane](../../1.pcb) and the
Simon's [end-to-end PCIE sim](../../5.sim), one of the project's three pillars.

- ✅ Hardware FSM replaced with an opensource RISC-V based SOC
- ✅ PCIE IP core replaced with opensource RTL
- ✅ Builds and runs with a **fully opensource toolchain** - link up at Gen2
- ✅ CSR generated with PeakRDL

---

## Status

| | |
|---|---|
| Target device | `xc7a200tfbg484-3`, Acorn CLE-215P |
| Link | **PCIE x4 Gen1** by default, **x1 Gen2** selectable. On hardware: x1 Gen2 works; x4 Gen2 trained (backplane RC4 ⇔ EP4 island), but its TLPs needed the new user clock, not re-tested |
| Note | x4 can only be tested in the **Direct** topology; [RC-switched](../3.Bonus--RC-switched.opensource) is x1 |
| Vivado build | works |
| Opensource build | works - see [`4.build/hw_build.openXC7`](../../4.build/hw_build.openXC7) |

Width and speed are `PCIE_LANES` and `PCIE_GEN`, read by [`link_pkg.sv`](../0.common.opensource/src/pcie/link_pkg.sv). Each build sets them and picks the matching lane XDC and bitstream name:

| | x4 Gen1 (default) | x4 Gen2 | x1 Gen2 |
|---|---|---|---|
| openXC7 | `make` -> `top.x4g1.direct.bit` | `make GEN=2` -> `top.x4g2.direct.bit` | `make LANES=1` -> `top.x1g2.direct.bit` |
| Vivado | `RC-direct.opensource.tcl` | `set ::pcie_gen 2` before sourcing it | `set ::pcie_lanes 1` before sourcing it |
| `run-lint.sh`, `run-leq.sh` | default | `GEN=2` | `LANES=1` |

They also set the user clock ([Link width, speed and user clock](../README.md#link-width-speed-and-user-clock)). CPU: openXC7 `make CPU=edubos5|edubos5_3stg|picorv32`, Vivado `set ::cpu ...`. Default: the 2-stage eduBOS5; at x4 Gen2 the 3-stage, which 250 MHz needs, with its multicycle constraint [`edubos5.3STG-ONLY.mcp.tcl`](../0.common.opensource/src/CPU.eduBOS5/edubos5.3STG-ONLY.mcp.tcl).

---

## Structure

The RTL is shared with [RC-switched](../3.Bonus--RC-switched.opensource), in [`0.common.opensource`](../0.common.opensource/README.md). This folder holds only the GT lanes:

```
2.rtl/2.RC-direct.opensource/     this folder
  xdc/
    RC-direct.x4g1g2.AcornCLE-215P.xdc GT lanes of the x4 link (default; any speed): X0Y6, X0Y4, X0Y5, X0Y7
    RC-direct.x1g2.AcornCLE-215P.xdc   GT lane of the x1 link: GTPE2_CHANNEL_X0Y5
  RC-direct.opensource.tcl        regenerates the Vivado project from scratch
```

### The PCIE stack, by layer

The `PCIE_2_1` hard block is instantiated directly. Everything around it, which AMD ships as encrypted IP, is opensource RTL here.

| Layer | Files | Role |
|---|---|---|
| Transaction | `pcie_core.sv` | `PCIE_2_1` instance and its TLP-side plumbing |
| Buffering | `buffer_bank.sv` | RX/TX packet buffers (BRAM) |
| Streaming | `trn_bridge.sv`, `trn_tx.sv`, `trn_rx.sv`, `trn_tx_throttle.sv` | AXI-Stream TLP interface towards the SOC |
| Bridge | `pcie_top.sv` | ties the transaction and PHY sides together |
| PHY control | `phy_top.sv`, `mac2phy.sv`, `gtp_wrapper.sv` | PIPE-level control (16/32-bit width conversion in `mac2phy`), reset and rate sequencing |
| Bring-up FSMs | `init_ctrl.sv`, `phase_align.sv`, `lane_xcvr.sv` | GT reset sequence, TX phase alignment, per-lane status |
| Transceiver | `lane_xcvr.sv`, `gtp_wrapper.sv` | `GTPE2_CHANNEL` and `GTPE2_COMMON` instances |
| Clocking | `clk_rst_gen.sv` | MMCM: 125 / 250 MHz and the user clock from the 100 MHz refclk |
| Tuning | `chan_retune.sv`, `speed_ctrl.sv`, `rx_eios_filter.sv` | RX data width (x16/x20) via DRP, Gen1/Gen2 rate change, EIOS filtering |
| Interfaces | `link_pkg.sv`, `pcie_pkg.sv`, `axis_if.sv`, `trn_if.sv`, `pipe_if.sv`, `cfg_if.sv`, `mem_if.sv`, `mim_if.sv`, `gtp_if.sv` | package and SystemVerilog interfaces, see [SystemVerilog interfaces](../README.md#systemverilog-interfaces) |

### Clocking

```
refclk_p/n (100 MHz PCIE refclk)
   └─ IBUFDS_GTE2 ── GTREFCLK0 ─ GTPE2_COMMON ─ PLL0 (2.5 GHz)

GTPE2_CHANNEL.TXOUTCLK ─ BUFG ─ MMCM ─┬─ CLKOUT0  125 MHz  ─ clk125 (DRP,
                                      │                      and PLL0 power-up)
                                      ├─ CLKOUT1  250 MHz  ─┐
                                      └─ CLKOUT2  62.5 / 125 / 250 MHz ─ user_clk (USERCLK, SOC)
                                                             │
                              BUFGCTRL mux (125 | 250) ─ pipe_clk = pipe clock
```

The pipe clock is 125 MHz at Gen1, 250 MHz at Gen2, switched by the BUFGCTRL mux on rate change. With `user_clk` at x4 Gen2, it is the tightest timing domain. `user_clk` follows the link: 62.5 MHz at x1, 125 MHz at x4 Gen1, 250 MHz at x4 Gen2 (`USER_CLK_FREQ` in `pcie_pkg.sv`). Full table: [Clocks and resets](../README.md#clocks-and-resets).

The PLL0 power-up delay in `gtp_wrapper` runs from `clk125`, not the refclk buffer: [why](../../4.build/hw_build.openXC7/README.md#pll0-power-up-delay-clocked-from-clk125).

---

### The CSR

The register window at `0x3000_0000` is **generated** by `peakrdl` from [`4.build/csr_build/csr.rdl`](../../4.build/csr_build/csr.rdl):

| Product | Where it lands | Who consumes it |
|---|---|---|
| `csr_pkg.sv`, `csr.sv` | `4.build/csr_build/generated-files/` | `soc_csr.sv` in this design |
| `csr.h` | same | the firmware in `3.sw/`, through `csr_regs.h` (also in co-simulation) |
| `openpcie.md`, `html/` | same | the documentation of the map itself |

`soc_csr.sv` is the only hand-written piece: it bridges the picorv32-style memory bus to the generated block's "passthrough" CPU interface, one request per CPU access. Regenerate after every edit of `csr.rdl`:

```bash
cd ../../4.build && make -f MakefileCSR
```

Map and flow: [`4.build/README.md`](../../4.build/README.md#csr-hal-compilation).

### The CPU

`riscv_pcie_soc.sv` picks the CPU by `ifdef`: eduBOS5 by default, `SOC_CPU_PICORV32` for PicoRV32 ([eduBOS5 README](../0.common.opensource/src/CPU.eduBOS5/README.md)), and `SOC_CPU_VPROC`. Only [`5.sim`](../../5.sim) sets the last: it swaps the core for a VProc virtual processor, so the firmware compiled for the host, or the rv32 ISS, drives the same bus. No bitstream flow defines it; the synthesised design always has a real core.

```sh
cd ../../5.sim
make run              # the eduBOS5 RTL core -- the default, and the reference
make run CPU=vproc    # the firmware, compiled for the host
make run CPU=iss      # rv32 ISS on the real firmware.elf
```

All three reach the same result over the same link. Run times: [The three CPU options](../../5.sim/README.md#the-three-cpu-options).

## Building

Run the [firmware build](../../4.build/sw_build) first: both flows need `firmware.hex`.

### Vivado

```tcl
source RC-direct.opensource.tcl
```

A short wrapper around the shared [`RC.opensource.tcl`](../0.common.opensource/RC.opensource.tcl): it collects `src/` and `src/pcie/*.sv` from `0.common.opensource`, the shared XDC plus this folder's lane XDC, and `firmware.hex` from `4.build/sw_build/` as memory-init file. The project lands in `xbuild.Vivado-v2024.2/`.

### Opensource

See [`4.build/hw_build.openXC7`](../../4.build/hw_build.openXC7).

----
End-of-Document
