# Simulation Models

**First rule: a model follows the RTL it stands in for as closely as possible** -- same ports, interfaces, structs, loops, names and widths. It differs only where it must (behaviour instead of hard macros), so an RTL change maps onto the model one to one.

| File | What it is |
|---|---|
| `soc_cpu.VPROC.native.sv` | _VProc_ as the SOC's CPU, on its **native** memory bus. The one in use: `make CPU=vproc` or `make CPU=iss` ([The three CPU options](../README.md#the-three-cpu-options)) |
| `soc_cpu.VPROC.sv` | inherited _VProc_ `soc_cpu` on the `soc_if` bus. Reference only, not instantiated |
| `phy_top.EP-PIPE-BFM.sv` | behavioural PIPE PHY in place of `2.rtl/0.common.opensource/src/pcie/phy_top.sv`; carries the _pcieVHost_ endpoint ([The PIPE PHY model](../README.md#the-pipe-phy-model)) |
| `PIPExN__pcieVHost.sv` | that endpoint in our wrapper: PIPE link, TLPs and DLLPs on one WaveCrux decoder page, x1 or x4 ([transaction taps](../README.md#every-layer-at-the-endpoint-transaction-taps)) |
| `pcieVHostPipexN.sv` | upstream's x1 PIPE wrapper `pcieVHostPipex1`, for any number of lanes; the x4 link |
| `pipe_x4_view.sv` | the four lanes of an x4 link, both directions, under the names the multi-lane PIPE and DLL decoders bind; dumped only at x4 |
| `bfm_uart.sv` | UART bus functional model; not instantiated by the current test bench |

| Sub-directory | Contents |
|---|---|
| [`cosim`](cosim/README.md) | _VProc_ and _mem_model_ co-simulation VIP |
| [`rv32`](rv32/README.md) | _rv32_ RISC-V RV32GC (+ Zba/Zbb/Zbc/Zbs) instruction set simulator, C++ |
| [`pcievhost`](pcievhost/README.md) | PCIe traffic generator with PIPE TX and RX data (x1 upstream; x4 through our `pcieVHostPipexN.sv`) |

## soc_cpu.VPROC.native

The CPU model in use. A drop-in for either RISC-V core (eduBOS5 by default, or PicoRV32), selected by `` `ifdef SOC_CPU_VPROC `` in `riscv_pcie_soc.sv`; nothing else changes. It presents picorv32's **native** bus ports -- `mem_valid`, `mem_ready`, `mem_addr`, `mem_wdata`, `mem_wstrb`, `mem_rdata` -- with a _VProc_ behind them.

* **Idle cycle between accesses.** _VProc_ holds `WE`/`RD` until it samples `WRAck`/`RDAck` at a rising edge, like `mem_valid`/`mem_ready`. Unlike picorv32 it does not guarantee an idle cycle between accesses, so a small state machine inserts one. Without it, back-to-back accesses can catch the trailing edge of the previous ready.
* **No instruction memory.** The program runs on the host, as native code or inside the _rv32_ ISS, so the SOC's 8 KB RAM holds no program. Only CSR accesses (`0x3000_0000`) reach this bus.

## soc_cpu.VPROC

Inherited from the sibling Chili.CHIPS SOC infrastructure. **Not used here**: this design has no `soc_if` bus, hence the native wrapper above.

* Pin compatible with the RTL softcore `soc_cpu` components (PicoRV32, EDUBOS5). A [_VProc_](https://github.com/wyvernSemi/vproc) runs natively compiled code in place of the softcore and drives the bus ([VProc Software](../README.md#vproc-software)).
* `soc_if`: the main memory mapped bus to the rest of the logic.
* `imem_we`, `imem_waddr[31:2]`, `imem_wdat[31:0]`: write-only port for program upload over UART. It feeds a [_mem_model_](https://github.com/wyvernSemi/mem_model) instance, _VProc_'s [main memory](../README.md#the-mem_model-co-simulation-sparse-memory-model), which uses the sparse C memory of [_pcieVHost_](https://github.com/wyvernSemi/pcievhost). The upload thus lands in the memory the virtual processor sees.

<p align="center"><img alt="soc_cpu.VPROC component" width="600" src="images/soc_cpu_vproc.png"></p>
