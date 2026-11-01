# RV32 ISS Model Integration for _openPCIE_

This directory runs the [rv32](https://github.com/wyvernSemi/riscV/tree/main/iss) RISC-V instruction set simulator (ISS) as a program on the [VProc virtual processor](https://github.com/wyvernSemi/vproc), in the test bench.

* ISS sources: `src` (upstream `774d84fa`, with `rv32-openpcie.patch`). The 5.sim make file builds the library.
* Integration code: `usercode`. It needs only the `rv32.h` header for all ISS features.

## File Organisation

| `usercode` files | Role |
|---|---|
| `VUserMain0.cpp`, `VUserMain0.h` | [main program](#main-program) and [callbacks](#user-callback-functions) |
| `vuserutils.cpp`, `vuserutils.h` | [utility functions](#utility-functions) |
| `rv32_timing_config.h` | timing models per core |
| `rv32_cache.cpp`, `rv32_cache.h` | cache hit/miss model |
| `mem_vproc_api.cpp`, `mem_vproc_api.h` | memory access: bus or direct |
| `uart.cpp`, `uart.h` | UART |

## Main Program

`VUserMain0` is the entry point of VProc node 0, the test bench CPU. It also defines the callbacks: ISS memory accesses (`ext_mem_access`), ISS interrupts (`iss_int_callback`) and VProc interrupts (`vproc_irq_callback`). The test bench does not use interrupts yet.

```
    Parse the vusermain.cfg file for integration and ISS configurations
    Create an rv32 ISS object
    Run pre-setup function to configure the model and the integration code (register callbacks, timings, cache, etc.)
    if GDB mode
        if specified, load an ELF executable program
        call the ISS GDB processing function to accept commands from an attached gdb until quit
    else
        load specified ELF executable program
        run the ISS until exit
    Run post run action function to display state and clean up
    clean up 
    Sleep program to allow simulation to continue
```

A class in `rv32_timing_config.h` sets the ISS timing for its seven instruction categories, from predefined models:

* PICORV32
* EDUBOS5 with 2 stage pipeline
* EDUBOS5 with 3 stage pipeline
* IBEX with single cycle multiplier
* IBEX with fast multi-cycle multiplier
* IBEX with slow multi-cycle multiplier

To add a core: add it here, and to `parseArgs` in `vuserutils.cpp` so `vusermain.cfg` can select it.

## User Callback Functions

### External Memory Access Callback

The ISS calls it on every memory access (loads, stores, atomics etc.) with an access type, an address, a reference to a data word, and its own time in cycles (from its timing model, including wait states returned by earlier calls).

```
    // Synchronisation
    Calculate the difference in ISS time reckoning since last call.
    if not doing a mem access over simulation bus
        if (difference greater than a maximum)
            Call 'tick' VProc API call for the difference number of cycles
    else
        Call 'tick' VProc API call for the difference number of cycles

    // Software interrupt
    if addr in s/w interrupt range and writing
        set the software irq state to data[0]

    // Memory access
    Switch on type 
       For data reads:
            Do read access for appropriate size
            Set read wait state value
       For Instruction reads:
            read word
            if enabled, update cache and get hit/miss and update 
            Set wait state based on hit/miss
        For data writes:
            Do write access for appropriate size
            Set read wait state value
        For invalid type:
            Flag access not processed by callback

    Return processed wait states or not processed
```

`mem_vproc_api.cpp` routes each access by address:

* **In the configured external range:** a VProc API call; the transaction appears on the CPU bus in the simulation.
* **Elsewhere:** a direct mem_model API call; no bus transaction. mem_model HDL instances still see the update, and the ISS sees HDL writes made through them.

`rv32_cache.cpp` holds no data. It tracks which addresses are cached, to give hit or miss, and updates its state on a miss.

### Interrupt Callbacks

`vproc_irq_callback` fetches the IRQ state from the HDL; `iss_int_callback` passes it, with the software interrupt state, to the ISS. Non-zero means active. Bits:

* bit 0: UART interrupt model
* bit 2: software interrupt
* others: generic external interrupts

## Utility Functions

`vuserutils.cpp` has three:

* **`parseArgs`** configures the ISS and the integration code, filling two configuration structures. VProc has no simulator command line, so `VUserMain0` passes argc 0 and argv `NULL`, and `parseArgs` reads `vusermain.cfg` instead: `vusermain<X> [options]`, X the node number (0 here). Options: [RISC-V Compiled Application](../../README.md#risc-v-compiled-application).
* **`reg_dump`** and **`csr_dump`** dump registers and CSRs in the post run actions (`post_run_actions` in `VUserMain0.cpp`) when the ISS exits.

For a run halted after a set number of instructions, the pre-run setup and post run actions time it and print the rate in MIPS.

## Test Example

`riscvtest/main.s` writes to memory over the HDL bus, reads back and checks. Build it with `rv_asm.sh`, or use the precompiled `main.bin`; select it in `vusermain.cfg` with `-t`.
