# _VProc_ and _mem_model_ Co-simulation Components

_VProc_ (virtual processor) and _mem_model_ (sparse memory, part of _pcievhost_) are the core of the openPCIE co-simulation. They share one memory:

* `soc_cpu.VPROC` is a _VProc_; its programs use the _mem_model_ space.
* Logic reaches the same space through the `mem_model` HDL component, also used in the PCIe interface driver.
* Code on the _pcievhost_ VIP's own _VProc_ reaches it through the API.

## _VProc_

_VProc_ runs a natively compiled user program on an HDL processor component. C/C++ APIs issue reads and writes on its HDL bus and advance simulation time.

<p align="center"><img alt="VProc virtual processor" width=450 src="https://github.com/user-attachments/assets/b272d61c-fa97-4ba2-a609-307ed4d0840c"></p>

* **Bus:** generic memory mapped. openPCIE's `soc_cpu.VPROC.native` wrapper turns it into picorv32's native interface (`mem_valid`/`mem_ready`) with a little logic.
* **Nodes:** instantiate as many as needed, each with a unique node number. Node *n* runs `VUserMain<n>` (e.g. `VUserMain0`), with C linkage for DPI.
* **Link to the simulator:** SystemVerilog DPI-C. The user program runs in its own thread, in lock-step with the simulation.
* **Build:** user programs and the _VProc_ library link into one shared object, `VProc.so`, loaded by the simulator at run time.

C and C++ APIs, abbreviated:

<p align="center"><img alt="VProc C and C++ API summary" width=700 src="https://github.com/user-attachments/assets/f8dd88fe-ed63-4a6c-a0eb-55a13d0f845d"></p>

User code runs in zero simulation time. Time advances on each transaction, or with `tick` without one. With these calls alone you can write test programs that drive the core's CSR registers from `soc_cpu.VPROC`, such as:

<p align="center"><img alt="A simple VProc test program" width=600 src="https://github.com/user-attachments/assets/a5b03827-e882-42d5-8765-c9cc3128b31a"></p>

## _mem_model_

[mem_model](https://github.com/wyvernSemi/mem_model) is a sparse memory written in C as part of [_pcievhost_](https://github.com/wyvernSemi/pcievhost), plus a `mem_model` HDL component with Avalon style ports (memory mapped, burst, write). _VProc_ code calls the C API directly:

<p align="center"><img alt="mem_model C API summary" width=400 src="https://github.com/user-attachments/assets/06278e89-e718-4396-9a05-d8c9cbb51efa"></p>

* Writes take `address`, `data` and, for wider words, a little-endian flag `le`. Reads take `address` (and `le` where needed) and return the value.
* Every function takes `node`: an address space, not the _VProc_ node. Almost always 0.
* The HDL component may be instantiated any number of times; all instances see the same memory, shared with every _VProc_.

<p align="center"><img alt="mem_model HDL component" width=400 src="https://github.com/user-attachments/assets/e991b49b-8b50-4e8f-bf7d-caa96992a680"></p>

## VProc and mem_model integration into _openPCIE_

This directory holds the HDL and libraries of both components. Compile `f_VProc.v` and `mem_model.sv`; they `` `include`` the rest (`f_mem_model.v`, the Verilog headers). Add the directory to the include path, as [`5.sim/tb.prj`](../../tb.prj) does:

```
sv work models/cosim/mem_model.sv
sv work models/cosim/f_VProc.v
```

The DPI software is pre-compiled into `lib/libcosimlnx.a`; headers are in `include/`. Natively compiled test code includes:

* `VProcClass.h` for the _VProc_ C++ API, or `VUser.h` for the C API;
* `mem.h` for the memory model C API.

From C++, wrap the C headers:

```
extern "C" {
#include "VUser.h"
#include "mem.h"
}
```

Application code compiled natively for _VProc_ gets these headers through the co-simulation HAL.

## More Information

[_VProc_ manual](https://github.com/wyvernSemi/vproc/blob/master/doc/VProc.pdf), [_mem_model_ manual](https://github.com/wyvernSemi/mem_model/blob/main/doc/mem_model_manual.pdf).
