# RISC-V Bare-Metal PCIE Driver

The **open-source driver** of the RISC-V Root Complex: C sources, startup assembly and linker script. It enumerates the link and runs memory transactions as the PCIE host. Build it in [`4.build/sw_build`](../4.build/sw_build) (`make`, or `make VARIANT=switched`).

<div align="center"><img src="../0.doc/3.sw/sw-structure.drawio.png" width="100%" alt="Firmware layers: application, PCIE driver, HAL, register map and startup, talking to the CSR block over the CPU memory bus, then through pcie_top to a direct endpoint or a switch with up to 4 endpoints"></div>

Diagram source: [`0.doc/3.sw/sw-structure.drawio`](../0.doc/3.sw/sw-structure.drawio).

## File Structure

| Directory | Contents |
| :--- | :--- |
| [`common/`](common) | `pcie.c` / `pcie.h`, `start.S`, `sections.lds` - used by both |
| [`RC-direct/`](RC-direct) | `main.c` for one Endpoint, straight RC-to-EP link |
| [`RC-switched/`](RC-switched) | `main.c` for the ASM1184e switch with up to 4 Endpoints behind it - see the [RC-switched README](../2.rtl/3.Bonus--RC-switched.opensource/README.md#2-firmware-the-bring-up-sequence) |

| File | Does |
| :--- | :--- |
| `common/pcie.c`, `common/pcie.h` | HAL and driver: register access, TLP send, Completion wait (with Completion Status and CRS retry), config read/write helpers, BAR sizing |
| `main.c` | **enumeration** (discovery, BAR placement, Command Register; `RC-switched` also the switch's bus numbers and windows) and the **app**: Memory Write, Memory Read, data check |
| `common/start.S` | sets the stack pointer, jumps to `main()` |
| `common/sections.lds` | maps code and data to the 8 KB BRAM at `0x00000000` |

The API and register map are common to both; the enumeration below is `RC-direct`'s.

### Code and data size

RV32I without C (no compressed instructions), `-O0`, riscv-none-elf-gcc 15.2; `riscv-none-elf-size -A` on the board build. Code, data and stack share the 8 KB SOC RAM; the stack starts at its top (`li sp, 0x2000` in `start.S`).

| Firmware | Code (`.text`) | Read-only data | Data (`.data` + `.bss`) | Stack, worst case | Of the 8 KB RAM |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `RC-direct` | 2,548 B | 0 B | 1 B | 368 B | 36% |
| `RC-switched` | 3,532 B | 8 B | 1 B | 416 B | 48% |

Data lives on the stack. The only global is `tx_tag` (`common/pcie.c`), the next request's tag; `RC-switched` adds two 4-byte constant tables of switch ports. Worst-case stack is the deepest call chain, from `gcc -fcallgraph-info=su`: `main` → `assign_bars` → `cfg_read32_checked` → `cfg_read32` → `pcie_cfg_read` → `pcie_request` → `wait_for_completion`. No recursion, no variable-size frames.

---

## API Reference

Four functions hide TLP construction and handshakes:

| Function | Does | Returns |
| :--- | :--- | :--- |
| `uint32_t pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t *val);` | Configuration Read: IDs, status, BARs. `*val` gets the raw (byte-swapped) value, only on success | Completion Status: `CPL_STAT_SC` (0), `_UR` (1), `_CRS` (2, after 100 retries), `_CA` (4), or `CPL_TIMEOUT` (8) |
| `uint32_t pcie_cfg_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t val);` | Configuration Write: BARs, Command register. A CRS answer is re-issued, as for reads | Completion Status, as above |
| `void pcie_mem_write(uint32_t addr, uint32_t val);` | Memory Write to BAR space, `addr` 4-byte aligned. Posted: returns at once, no Completion | - |
| `uint32_t pcie_mem_read(uint32_t addr, uint32_t *val);` | Memory Read from BAR space. `*val` written only on success | Completion Status, as above |

`bus, dev, func` is usually `1, 0, 0` on a direct link. The three non-posted calls share `pcie_request()`. `main()` uses the wrappers `cfg_read32()` / `cfg_write32()`, which add `bswap32()` and, for writes, stop with `0xBAD00002` on failure.

```c
if (pcie_cfg_read(1, 0, 0, 0x00, &id) == CPL_STAT_SC) ...   // Vendor ID
status = pcie_cfg_write(1, 0, 0, 0x10, 0xFFFFFFFF);         // size BAR0
pcie_mem_write(0x80000000, 0x00000006);
if (pcie_mem_read(0x80000000, &data) == CPL_STAT_SC) ...
```

---

## Hardware Abstraction Layer (HAL)

The driver talks to the PCIE bridge RTL through memory-mapped registers.

### Register Map
`common/pcie.h` takes these from `csr.h` (generated from `csr.rdl`) through `csr_regs.h`; see [4.build/README.md](../4.build/README.md#the-register-map).

| Register Name | Address | R/W | Description |
| :--- | :--- | :--- | :--- |
| `PCIE_TX_HEADER0` | `0x30000000` | W | TLP Header DW0 (Type, Fmt, Length). |
| `PCIE_TX_HEADER1` | `0x30000004` | W | TLP Header DW1 (Requester ID, Tag, Byte Enables). |
| `PCIE_TX_HEADER2` | `0x30000008` | W | TLP Header DW2 (Target Address or Bus/Dev/Func). |
| `PCIE_TX_DATA` | `0x3000000C` | W | **Write:** Data Payload. |
| `PCIE_RX_STATUS` | `0x30000010` | R | Completion Status (`0`=Success, `1`=UR, `2`=CRS, `4`=CA). |
| `PCIE_RX_DATA` | `0x30000014` | R | Data received from Memory Read Completions. |
| `PCIE_RX_HEADER_INFO`| `0x30000018` | R | **Completion Info:** Requester ID, Tag and Lower Address, for matching. |
| `PCIE_ERR_STATUS` | `0x3000001C` | R | **Error Status:** a flag for a received fatal-error Message (bit 16). The `PCIE_2_1` hard block does not export its PCI Status register, so bits [15:0] read 0. |
| `PCIE_PHY_STATUS` | `0x30000020` | R | Tx FSM state and the number of free hard-macro Tx buffers - polled before every send. |
| `PCIE_RESULT` | `0x30000024` | RW | **Result marker** the firmware writes when it stops (table below). Sends nothing; drives the PASS LED and an ILA probe. |

---

## Driver Logic & Features

### 1. Robust TLP Transmission
Every non-posted request (config read, config write, memory read) goes through `pcie_request()`. It checks the **Completion Status** (SC / UR / CRS / CA) or reports a timeout, and returns read data separately, so a genuine `0xFFFFFFFF` is never taken for an error. A **CRS** answer (Configuration Retry Status), read or write, is re-issued with a fresh tag up to 100 times. A config request that fails after the device was found stops the firmware with `0xBAD00002`.

### 2. Enumeration Sequence (in `main`)
1.  **Wait** for the link to settle.
2.  **Discovery:** read the `Device ID` on bus 1.
3.  **BAR sizing:** with memory decoding off, write `0xFFFFFFFF` to each of the six BARs and read back. `0` = not implemented; bit 0 = I/O BAR (left unassigned: this RC has no I/O space); bits `[2:1] = 10` = 64-bit BAR, the next BAR is its upper half. Size = `~(readback & ~0xF) + 1`.
4.  **Assignment:** place each memory BAR, aligned to its size, in `0x80000000 - 0xFFFFFFFF` (`RC-switched`: the 1 MB window of the endpoint's switch port). BAR0 lands at the base: `0x80000000` for the AMD EP's 2 KB BAR0. A BAR that does not fit is parked (64-bit above 4 GB, 32-bit at 0); if it is BAR0, the firmware stops with `0xBAD00003`.
5.  **Enable:** set **Bus Master** and **Memory Space** in the Command Register.

Configuration payloads are big-endian on this RC (register byte 0 in bits `[31:24]`), so config data goes through `bswap32()` and source values read as the spec prints them.

### 3. Self-Test & Debug Codes
The driver ends by writing a marker to `PCIE_RESULT`. Unlike `PCIE_TX_DATA`, this sends no TLP. On the board, PASS lights `led_link_up[3]`, and an ILA can watch every marker ([RTL Debug with ILA](../2.rtl/README.md#rtl-debug-with-ila)).

| Magic Number | Meaning |
| :--- | :--- |
| **`0x0000FACE`** | **PASS:** Data `0x6` was written and successfully read back. |
| **`0x0000DEAD`** | **FAIL:** Readback data did not match the written value, or the memory read was not completed with SC. |
| **`0xBAD00000`** | **ERROR:** Device ID read failed: no device at bus 1, or no Completion. (Not a link down: the SOC is held in reset until the link is up.) In `RC-switched`: the switch upstream port did not answer. |
| **`0xBAD00001`** | **ERROR** (`RC-switched` only): the switch is up, but no Endpoint was found behind it. |
| **`0xBAD00002`** | **ERROR:** A config request to a device that had answered failed (UR, CA, timeout, or CRS after 100 retries). |
| **`0xBAD00003`** | **ERROR:** The Endpoint's BAR0 is not implemented, is an I/O BAR, or does not fit the window. |


----------------
#### End-of-Document
