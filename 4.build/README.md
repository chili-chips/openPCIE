# _openPCIE_ Build

Three steps, run in order, each with its own `Makefile`. The firmware uses the generated HAL; the bitstream embeds the firmware.

## Build environment

Linux-native tools; WSL works too. Only WaveCrux runs on Windows, where GPU drivers make GUIs smoother. First, in this folder:

```sh
source set-env.sh       # bash, zsh
source set-env.csh      # tcsh, csh
```

[`set-env.sh`](./set-env.sh) finds each tool, checks it against [Tool versions](#tool-versions), and exports the variable the Makefiles use (`YOSYS`, `RISCV_PREFIX`, `PEAKRDL`, ...). It lists what is missing or too old and how to install it. Windows `.exe` files on `PATH` count as missing.

- Report only: `bash set-env.sh`
- Pin a tool: `export YOSYS=/path/to/yosys` before sourcing, or `make YOSYS=...`
- Search roots: `OPENXC7_ROOT`, `XILINX_VIVADO`, `RISCV_ROOT`; else `~/opt`, `/opt`, `/tools/Xilinx`, `~/Xilinx`

| Step | Makefile / Directory | Produces |
|---|---|---|
| 1. CSR | [`MakefileCSR`](./MakefileCSR) | `csr_build/generated-files/` - `csr.sv`, `csr_pkg.sv`, `csr.h`, `csr_top.h`, register map docs |
| 2. SW | [`sw_build`](./sw_build) | `firmware.elf`, `firmware.bin`, `firmware.hex` |
| 3. HW | [`hw_build.openXC7`](./hw_build.openXC7) | `build_artifacts/top.x4g1.direct.bit` |

<div align="center"><img src="../0.doc/4.build/build-flow.drawio.png" width="100%" alt="Build flow: csr.rdl through PeakRDL into the register RTL and the firmware and co-sim headers; the firmware build into firmware.hex; the bitstream build through Vivado or openXC7 (slang or sv2v, yosys, nextpnr, fasm2frames and xc7frames2bit) into top.<link>.<variant>.bit for the Acorn CLE-215+; co-simulation, lint and the formal equivalence check alongside"></div>

Diagram source: [`0.doc/4.build/build-flow.drawio`](../0.doc/4.build/build-flow.drawio).

## Tool versions

Validated versions. "Board": bitstream tested on hardware; "builds": completes, not on the board yet.

| Tool | Version | Used for | Status |
|---|---|---|---|
| openXC7 v1.0.0 (1) | Yosys 0.69, nextpnr 3e5c2cdd | bitstream, default (`read_slang`) | builds, x4 and x1 |
| openXC7 pre-v1.0.0 (2) | Yosys 0.38, nextpnr-xilinx 45a986b | `build_artifacts/pre-v1.0.0/`, flow removed after `54c5409` | board, x1 |
| sv2v | v0.0.13 | `FRONTEND=sv2v` | builds |
| AMD Vivado | 2026.2 (2026.1 or later) | co-sim (`xsim`), `run-lint.sh` cdc, check-timing and methodology phases, implementation | runs, checks |
| PeakRDL | 1.5.0 (3) | CSR RTL, headers, docs | generates |
| xPack RISC-V GCC | 15.2.0-1 | firmware | board |
| GCC, host | 10+ (4) | co-sim (VProc, pcievhost) | runs |
| Verilator | 5.055 (devel, master `v5.054-420-g1b3f232f`, 2026-11-27; CI builds the same) | `run-lint.sh` | checks |
| JanusHDL | 0.22.0 | co-sim of the RC model (`make SIM=janushdl RC=model run`); free, closed source | runs: `PHY=none` x1 and x4, `PHY=core` x1 pass |
| Verible | v0.0-4302-gc6b29d62 | `run-format.sh`, `run-lint.sh` | formats, checks |
| ABC (`yosys-abc`) | 1.01, in openXC7's Yosys 0.69 | formal: eduBOS5 multicycle proof (`pdr`), bounded runs (`bmc3`) | proves |
| SymbiYosys (`sby`) | v0.69 | formal: riscv-formal on eduBOS5, with ABC `bmc3` | checks |
| Z3 | 5.3.1 (pip `z3-solver`) | formal: replays a riscv-formal failure | runs |
| WaveCrux Pro | 1.1.1 | waveforms, RISC-V decoding (built in) | runs |
| Ferrite wavecrux-decoders | pcie-v0.2.0, pcie-tlp-v0.1.1 (5) | PIPE, DLL and TLP decoding in WaveCrux | runs |
| Python | 3.12 | build scripts | runs |
| openFPGALoader | 1.1.1 | loading the bitstream over JTAG | not yet used on the board |
| usbipd-win | 5.3.0 | JTAG cable into WSL (Windows side) | not yet used on the board |
| usbutils (`lsusb`) | 017 | checking the cable in WSL | runs |

1. Yosys 0.69 (9f75ca1f9), nextpnr-himbaechel 1.0.0-41-g3e5c2cdd, fpga-as
2. Yosys, nextpnr-xilinx, prjxray
3. regblock 1.3.1, html 2.12.2, markdown 1.0.3, c-header 1.1.0; systemrdl-compiler 1.33.0
4. C++20; validated with 15.2.0
5. `pcie-tlp` is our TLP plugin, contributed to Ferrite's wavecrux-decoders

## CSR HAL Compilation

```sh
make -f MakefileCSR          # only after editing csr_build/csr.rdl
make -f MakefileCSR tools    # which peakrdl is used
```

`peakrdl` turns one SystemRDL file, `csr_build/csr.rdl`, into the CSR RTL and the firmware's register header, in `4.build/csr_build/generated-files`:

  * `csr.sv`      : the CSR RTL
  * `csr_pkg.sv`  : its `hwif_in` / `hwif_out` struct package
  * `csr.h`       : `peakrdl c-header` output: the register structs, field masks and positions
  * `csr_top.h`   : `CSR_TOP_T`, the address map's struct in `csr.h`
  * `openpcie.md` : the register map as markdown
  * `html/`       : the register map as a browsable HTML document, not in git

All but `html/` are checked in; an ordinary build never regenerates them.

The firmware includes [`csr_regs.h`](csr_build/csr_regs.h), next to `csr.rdl` (the same file in openPCIE, openCologne-PCIE and WireGuard-FPGA). It takes the layout and CSR base address from `csr.h` and gives the accessors, for the RISC-V cores and co-simulation alike: `CSR_REG32()` for a whole register, `CSR_GET()`, `CSR_SET()` and `CSR_RMW()` for one field:

```c
    PCIE_TX_PAYLOAD(0) = h0;              // CSR_REG32(tx_payload.mem[0])
    uint32_t phy       = PCIE_PHY_STATUS; // CSR_REG32(status.phy)
    uint32_t bufs      = RDL_FIELD(phy, CSR__STATUS__PHY__TX_BUF_AVAIL);
```

With `make CPU=vproc` in [5.sim](../5.sim/README.md#the-three-cpu-options) the same firmware is compiled for the host, and `CSR_REG32()` becomes a *VProc* read or write on the same CPU bus. One firmware source, one register map.

On a RISC-V core, `CSR_REG32()` takes the register's address from the generated struct (`CSR_ADDR()`) and makes one aligned 32-bit access. Do **not** dereference the struct itself: `peakrdl c-header` marks every struct `__attribute__((packed))`, so on a target without unaligned load/store (rv32i, for one) a register access compiles into four byte accesses:

```
    lbu a4,12(a5)   /   sb a4,12(a5)   /   lbu a4,13(a5)   ...
```

Wrong for a CSR: the write strobe of `tx.send` starts a TLP transfer, so the packets leave after the first byte. Whether the compiler merges the bytes depends on the optimisation level, so `CSR_REG32()` is the only safe access at any `-O`, the board build's `-Os -flto` included. On x86 you never see it; it took a run on the real ISA.

### The register map

`csr.rdl` describes the TLP transmit/receive window the firmware uses in place of the AMD-proprietary hardware FSM, at `0x3000_0000` in the SOC address map. The layout is openCologne-PCIE's: every TLP, header and payload, goes through a payload window, so a Memory request can carry a burst of up to 32 DW (Max_Payload_Size, 128 bytes):

| Offset | Register | Access | Meaning |
|---|---|---|---|
| `0x0C` | `tx.send` | W | send N TLPs from `tx_payload` - **writing it starts the transfer** |
| `0x20` | `rx.valid` | RW | 1 while a TLP waits at the head of the RX ring; writing 1 frees it |
| `0x24` | `status.err` | RW1C | fatal-error message flag |
| `0x28` | `status.phy` | R | `tx_busy` + free hard-macro Tx buffers |
| `0x2C` | `status.result` | RW | firmware result marker (`0x0000FACE` = PASS); sends nothing, drives the PASS LED |
| `0x1000`-`0x1FFC` | `rx_payload` | R | the RX ring's head TLP, as it came, header first: entry i is DW i |
| `0x2000`-`0x2FFC` | `tx_payload` | W | TLPs to send, back to back from entry 0, header first |

- **Each payload window is one RAMB36**, 1024 x 32, true dual port: a 64-bit beat writes (Rx) or reads (Tx) both its DWORDs in one clock.
- **Tx is a batch.** The firmware fills `tx_payload`, then writes the number of TLPs into `tx.send` (an external register: nothing is stored). Each TLP's DW0 gives its length, so the next starts right after it. The RAM is read ahead into a 4-beat FIFO, so the core's `tready` reaches no RAM address: that path did not fit x4 Gen2's 250 MHz.
- **Rx is a ring.** Every TLP the hard macro passes on queues up at link speed; `m_axis_rx.tready` drops only while the ring is full (511 beats), so nothing is lost. Completions, Messages and an endpoint's own requests all land there; the firmware frees what it does not use.
- **Timing at 250 MHz** (x4 Gen2): CSR reads answer from a flop (PeakRDL `--rt-read-response`), an `rx_payload` read a clock later still, and the ring's `tready` comes from a flop. The CPU address picks the CSR with bit 28 alone, as in openCologne-PCIE.
- **The firmware builds every header**, the Type 0 / Type 1 choice of a Configuration request included ([3.sw](../3.sw/README.md)).

The [co-simulation](../5.sim) runs the real firmware against a PCIE endpoint model on this register block, ending with the memory read-back check passing.

## SW Compilation

```sh
cd sw_build && make             # RC-direct
make VARIANT=switched           # RC-switched
```

Builds the [`3.sw`](../3.sw) sources (xPack `riscv-none-elf-` GCC, `rv32im`, or `rv32i` with `MUL=0`) into `firmware.elf`, `.bin` and `.hex` in `4.build/sw_build`, using the step 1 headers. The hardware build embeds `firmware.hex`. Every `make` rebuilds (about a second), so switching variants needs no `make clean`. Details: [`sw_build/README.md`](./sw_build/README.md).

## HW Compilation

Two independent flows, same device: `xc7a200tfbg484-3` on the Acorn CLE-215+.

### Proprietary AMD flow

- **AMD designs:** Vivado GUI, with the project files in each design folder of `2.amd-rtl-with-Vivado-build`, e.g. [2.RC-direct.amd/xbuild.Vivado-v2024.2](../2.amd-rtl-with-Vivado-build/2.RC-direct.amd/xbuild.Vivado-v2024.2).
- **Opensource RTL in Vivado:** [`RC-direct.opensource.tcl`](../2.rtl/2.RC-direct.opensource/RC-direct.opensource.tcl) and [`RC-switched.opensource.tcl`](../2.rtl/3.Bonus--RC-switched.opensource/RC-switched.opensource.tcl), two short wrappers around the shared [`RC.opensource.tcl`](../2.rtl/0.common.opensource/RC.opensource.tcl).

### Opensource openXC7 flow

In [`hw_build.openXC7`](./hw_build.openXC7) ([README](./hw_build.openXC7/README.md)); no proprietary tool in the chain. Default: openXC7 v1.0.0.

| Build | Chain | Output |
|---|---|---|
| `make` | `yosys` (`read_slang`, no sv2v) -> `nextpnr-himbaechel` -> `fasm2frames` -> `xc7frames2bit` (1) | `build_artifacts/top.x4g1.direct.bit` |
| `make GEN=2` | same, x4 Gen2, 3-stage eduBOS5; misses 250 MHz ([timing gap](#openxc7-timing-closure-gap)) | `build_artifacts/top.x4g2.direct.bit` |
| `make LANES=1` | same, x1 link | `build_artifacts/top.x1g2.direct.bit` |
| `make FRONTEND=sv2v` | `sv2v` -> `yosys` -> `nextpnr-himbaechel` -> `fasm2frames` -> `xc7frames2bit` (1) | `build_artifacts/work/x4g1.direct.sv2v/top.x4g1.direct.bit` |

1. `ASSEMBLER=fpga-as` for the one-step assembler. Not the default yet: it writes `RXCDR_CFG` wrong ([#12](https://github.com/chili-chips/openPCIE/issues/12)).

Add `VARIANT=switched` for RC-switched: `build_artifacts/top.x1g2.switched.bit`. Each build has its own work folder and makes the firmware of its variant, so a bitstream always carries the matching one. `make check-env` checks the tools.

**Verified on hardware:** the pre-v1.0.0 bitstream (`build_artifacts/pre-v1.0.0/`) brings the link up at **Gen2** against a second Artix-7 board as endpoint, as does the Vivado bitstream from the same RTL. The v1.0.0 bitstreams build; not on the board yet.

#### openXC7 timing closure gap

The same RTL that closes in Vivado misses timing in openXC7 at x4 Gen2. `user_clk`, which clocks the SOC and the TLP interface:

| Build, `user_clk` target | Vivado 2026.2 | openXC7 main (2026-11-27, seed 3) |
|---|---|---|
| x4 Gen2, 250 MHz | **meets**, WNS +0.036 ns | **152 MHz**, misses (seeds 1-3: 116-152) |
| x4 Gen1, 125 MHz | meets | 139 MHz, meets |
| x1 Gen2, 62.5 MHz | meets | 121 MHz, meets |

- **Multicycle paths.** nextpnr cannot apply the eduBOS5 3-stage multicycle paths ([nextpnr#182](https://github.com/openXC7/nextpnr/issues/182)). Vivado takes them from `edubos5.3STG-ONLY.mcp.tcl`. The critical path starts at the CPU's data address, routed across 24 rows.
- **No physical optimisation after routing.** Vivado's last 0.119 ns came from post-route `phys_opt` (in `RC.opensource.tcl`), on 2 cells. nextpnr has none: proposed as [openXC7/nextpnr#251](https://github.com/openXC7/nextpnr/issues/251) and [YosysHQ/nextpnr#1954](https://github.com/YosysHQ/nextpnr/issues/1954).
- **`make TIMING_OPT=3`** runs openXC7's critical-path placement pass (`-o timing-opt`): seed 2 163.43 MHz (+9%), seed 3 164.77 MHz (+8%), seed 1 101.69 MHz (-12%). Not enough, and not stable.
- **Block RAM reads** are now timed ([nextpnr#190](https://github.com/openXC7/nextpnr/pull/190)); since the SOC's 250 MHz fixes they are no longer the critical path.

For x4 Gen2, use Vivado until openXC7 closes this gap.

Before the first build ([details](./hw_build.openXC7/README.md)):

- **Bitstreams are not compressed:** 9.73 MB each; neither openXC7 assembler compresses yet ([lromor/fpga-assembler#66](https://github.com/lromor/fpga-assembler/issues/66)). The Vivado build compresses.
- **nextpnr's GT attribute defaults differ from the Xilinx library.** For an attribute the RTL leaves out, Vivado uses the `unisim` default, nextpnr its own, almost always `0`, silently. This is why the design first failed on hardware. `lane_xcvr.sv` now sets 20 of them explicitly.

## Loading the bitstream

[openFPGALoader](https://github.com/trabucayre/openFPGALoader) (open source, v1.1.1+), over JTAG:

```sh
openFPGALoader --detect                                    # cable and FPGA found?
openFPGALoader hw_build.openXC7/build_artifacts/top.x4g1.direct.bit
```

Ubuntu's package is older: build it from source. On WSL, attach the JTAG cable with usbipd-win first. `set-env.sh` checks both ([Programming the board from WSL](./hw_build.openXC7/README.md#programming-the-board-from-wsl)).

-----------
#### End-of-Document
