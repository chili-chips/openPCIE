# Simulation Models

**First rule: a model follows the RTL it stands in for as closely as possible** -- same ports, interfaces, structs, loops, names and widths. It differs only where it must (behaviour instead of hard macros), so an RTL change maps onto the model one to one.

| File | What it is |
|---|---|
| `soc_cpu.VPROC.native.sv` | _VProc_ as the SOC's CPU, on its **native** memory bus. The one in use: `make CPU=vproc` or `make CPU=iss` ([The three CPU options](../README.md#the-three-cpu-options)) |
| `soc_cpu.VPROC.sv` | inherited _VProc_ `soc_cpu` on the `soc_if` bus. Reference only, not instantiated |
| `phy_top.EP-PIPE-BFM.sv` | behavioural PIPE PHY in place of `2.rtl/0.common.opensource/src/pcie/phy_top.sv`; carries the _pcieVHost_ endpoint ([The PIPE PHY model](../README.md#the-pipe-phy-model)) |
| `pcie_core.RC-model.sv` | the Artix-7 RC model in the hard macro's socket: `pcie_core`'s ports, the model and `pipe_mac_adapter.sv` inside (`tb_socket.sv`, `PHY=hm`) |
| `bfm_uart.sv` | UART bus functional model; not instantiated by the current test bench |

The PCIE model is [openPCIE_BFM](https://github.com/chili-chips/openPCIE_BFM), the submodule `../vip/openPCIE_BFM`: pcieVHost, VProc and mem_model, our PIPE wrappers (`PIPExN__pcieVHost.sv`, `pcieVHostPipexN.sv`, `pipe_phy_core.sv`, `pipe_mac_adapter.sv`, `pipe_sym_pkg.sv`, `pipe_x4_view.sv`) and the Artix-7 RC model. Its README has the details; the files here use it.

| Sub-directory | Contents |
|---|---|
| [`rv32`](rv32/README.md) | _rv32_ RISC-V RV32GC (+ Zba/Zbb/Zbc/Zbs) instruction set simulator, C++ |

## soc_cpu.VPROC.native

The CPU model in use. A drop-in for either RISC-V core (eduBOS5 by default, or PicoRV32), selected by `` `ifdef SOC_CPU_VPROC `` in `riscv_pcie_soc.sv`; nothing else changes. It presents picorv32's **native** bus ports -- `mem_valid`, `mem_ready`, `mem_addr`, `mem_wdata`, `mem_wstrb`, `mem_rdata` -- with a _VProc_ behind them.

* **Idle cycle between accesses.** _VProc_ holds `WE`/`RD` until it samples `WRAck`/`RDAck` at a rising edge, like `mem_valid`/`mem_ready`. Unlike picorv32 it does not guarantee an idle cycle between accesses, so a small state machine inserts one. Without it, back-to-back accesses can catch the trailing edge of the previous ready.
* **No instruction memory.** The program runs on the host, as native code or inside the _rv32_ ISS, so the SOC's 8 KB RAM holds no program. Only CSR accesses (`0x3000_0000`) reach this bus.

## soc_cpu.VPROC

Inherited from the sibling Chili.CHIPS SOC infrastructure. **Not used here**: this design has no `soc_if` bus, hence the native wrapper above.

* Pin compatible with the RTL softcore `soc_cpu` components (PicoRV32, EDUBOS5). A [_VProc_](https://github.com/wyvernSemi/vproc) runs natively compiled code in place of the softcore and drives the bus ([VProc Software](../README.md#vproc-software)).
* `soc_if`: the main memory mapped bus to the rest of the logic.
* `imem_we`, `imem_waddr[31:2]`, `imem_wdat[31:0]`: write-only port for program upload over UART. It feeds a [_mem_model_](https://github.com/wyvernSemi/mem_model) instance, _VProc_'s [main memory](../README.md#the-mem_model-co-simulation-sparse-memory-model), which uses the sparse C memory of [_pcieVHost_](https://github.com/wyvernSemi/pcievhost). The upload thus lands in the memory the virtual processor sees.

<div align="center"><img alt="soc_cpu.VPROC component" width="600" src="images/soc_cpu_vproc.png"></div>

## VProc and mem_model

_VProc_ (virtual processor) and _mem_model_ (sparse memory, part of _pcievhost_) are the core of the openPCIE co-simulation. Both come from openPCIE_BFM. They share one memory:

* `soc_cpu.VPROC` is a _VProc_; its programs use the _mem_model_ space.
* Logic reaches the same space through the `mem_model` HDL component, also used in the PCIe interface driver.
* Code on the _pcievhost_ VIP's own _VProc_ reaches it through the API.

How they work, their C and C++ APIs: [openPCIE_BFM, VProc and mem_model](https://github.com/chili-chips/openPCIE_BFM/blob/main/2.rtl/README.md#vproc-and-mem_model).

### Their use in _openPCIE_

`make` builds both from openPCIE_BFM into `5.sim/obj/vip`: `lib/libcosimlnx.a` (VProc), `include/` (the C headers), `hdl/` (the HDL). The `.prj` files compile openPCIE_BFM's `2.rtl/mem_model.sv` and `hdl/f_VProc.sv`; they `` `include`` the rest (`f_mem_model.sv`, the Verilog headers) through the include path. Natively compiled test code includes:

* `VProcClass.h` for the _VProc_ C++ API, or `VUser.h` for the C API;
* `mem.h` for the memory model C API.

From C++, wrap the C headers:

```
extern "C" {
#include "VUser.h"
#include "mem.h"
}
```

Application code compiled natively for _VProc_ gets these headers through the co-simulation HAL. More: the [_VProc_ manual](https://github.com/wyvernSemi/vproc/blob/master/doc/VProc.pdf), the [_mem_model_ manual](https://github.com/wyvernSemi/mem_model/blob/main/doc/mem_model_manual.pdf).
