# firmware build

Builds the firmware for the RISC-V core (eduBOS5 or PicoRV32) of the opensource RC, `RC-direct.opensource` or `RC-switched.opensource`:

```sh
cd 4.build
source set-env.sh        # tcsh: source set-env.csh
cd sw_build
make                     # RC-direct
```

Run it on Linux or in WSL, **before** either hardware build: both need `firmware.hex`.

| Stage | Tool | Input -> Output |
|---|---|---|
| 1. Compile + link | `riscv-none-elf-gcc` | `start.S` + `pcie.c` + `main.c` -> `firmware.elf` |
| 2. Raw image | `riscv-none-elf-objcopy` | `.elf` -> `firmware.bin` |
| 3. Hex for `$readmemh` | `python` | `.bin` -> `firmware.hex` |

Sources live in [`3.sw/`](../../3.sw/README.md): `common/` for both variants, plus the variant's `main.c`. Outputs land here, next to the Makefile.

| `VARIANT` | `main.c` from | Topology |
|---|---|---|
| `direct` (default) | `3.sw/RC-direct/` | one endpoint, straight RC-to-EP link |
| `switched` | `3.sw/RC-switched/` | ASM1184e switch with up to 4 endpoints |

The register map comes from `../csr_build/generated-files/` (`csr.h`, `csr_top.h`), generated from `csr.rdl`, and the accessors from `../csr_build/csr_regs.h` - see [`4.build/README.md`](../README.md#csr-hal-compilation).

---

## Prerequisites

- **RISC-V toolchain:** [xPack GNU RISC-V Embedded GCC](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack), prefix `riscv-none-elf-`, `linux-x64` build, unpacked e.g. into `~/opt`. [`../set-env.sh`](../set-env.sh) finds it and sets `RISCV_PREFIX` to its full prefix. Other toolchains: `make CROSS=riscv64-unknown-elf-`.
- **Python 3:** any version; only `struct` is used.

## Building

| Target | Purpose |
|---|---|
| `make` | build `firmware.elf`, `firmware.bin`, `firmware.hex` for RC-direct |
| `make VARIANT=switched` | the same three files, but for RC-switched |
| `make SIM=1` | short start-up delay, for co-simulation (see below; `5.sim` does this itself) |
| `make COUNTERS=0` | waits count loops, for a CPU without `rdcycle` ([below](#timing-the-waits)) |
| `make OUT=<dir>` | write the three files to `<dir>` instead of this folder |
| `make info` | print the resolved configuration |
| `make clean` | remove the three generated files |

**Every `make` rebuilds from scratch** (about a second). **No `make clean` needed, ever.** Why: both variants write the same output names, and `VARIANT` and `SIM` leave no trace in the sources, so a date-based make would keep the **previous** image. File dates on a network share, or copied off one, can also be in the future.

## Timing the waits

The firmware waits three times ([`3.sw/common/pcie.h`](../../3.sw/common/pcie.h)), in µs:

| Wait | Board | `SIM=1` | Why |
|---|---|---|---|
| `STARTUP_US`: before the first Configuration Request | 100 ms | 75 µs | PCIe Base 6.6.1: 100 ms after reset |
| `CRS_WAIT_US`: between re-issues after CRS (100 tries) | 10 ms | 374 µs | 6.6.1: a device may answer CRS for 1 s |
| `CPL_TIMEOUT_US`: no Completion, give up | 50 ms | 2.18 ms | 2.8: the end of the default Completion Timeout range |

`COUNTERS` picks how they are timed, and must match the hardware: [`5.sim`](../../5.sim) and [`hw_build.openXC7`](../hw_build.openXC7) pass theirs, and every build, the Vivado flow ([`RC.opensource.tcl`](../../2.rtl/0.common.opensource/RC.opensource.tcl)) too, passes its `LINK`.

* `COUNTERS=1` (default): the CPU's cycle counter (`rdcycle`). Real time at any `-O`. `LINK` gives the clock: 62.5 MHz at x1, 125 MHz at x4 Gen1, 250 MHz at x4 Gen2.
* `COUNTERS=0`: loops, calibrated on eduBOS5 at x1 without `-O` (374 ns a loop, 1.09 µs a poll). An `-O` or a faster clock shortens them.

## Building for co-simulation

`make SIM=1` shortens the waits (table above), so a run fits in under 0.7 ms of simulated time (0.4 ms on eduBOS5, 0.66 ms on PicoRV32). The hardware build is unaffected.

You do not run it by hand: `make` in [`5.sim`](../../5.sim) builds its own copy every time, with `make SIM=1 OUT=5.sim/obj/fw`. So the `firmware.hex` tracked here is always the board build.

`SIM=1` matters for `CPU=rtl` and `CPU=iss`, which run this image. `CPU=vproc` compiles the sources for the host; its waits count loops, which take no simulated time.

## Outputs

| File | Used by |
|---|---|
| `firmware.elf` | debugging / `readelf`, `objdump`, **and the rv32 ISS** in co-simulation - `make CPU=iss` in [`5.sim`](../../5.sim/README.md#the-three-cpu-options) loads it directly |
| `firmware.bin` | intermediate, raw image |
| **`firmware.hex`** | **the hardware build** - both Vivado and openXC7, and the RTL core in co-simulation |

`riscv_pcie_soc.sv` loads its RAM with `$readmemh("firmware.hex", ram)`, so the file is one 32-bit little-endian word per line, 8 hex digits, no `0x` prefix. Step 3 emits exactly that.

## Notes

- `-march=rv32i -mabi=ilp32` only: eduBOS5 is RV32I, and PicoRV32 here has no M or C extension.
- `-ffreestanding -nostdlib`: no libc, no crt0. `start.S` provides the reset vector, so it stays first on the command line.
- `firmware.elf` differs on every rebuild: gcc embeds a randomly named temporary object for the `start.S` pass in the symbol table. `firmware.bin` and `firmware.hex`, the ones that matter, are stable.

-----------
#### End-of-Document
