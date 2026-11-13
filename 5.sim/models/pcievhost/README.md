# The _pcievhost_ Verification IP

## Table of Contents

* [Introduction](#introduction)
* [The HDL Component](#the-hdl-component)
  * [Special values on the line](#special-values-on-the-line)
* [Link Traffic Display](#link-traffic-display)
* [The Pcie C Model](#the-pcie-c-model)
  * [Internal Architecture](#internal-architecture)
  * [User API Summary](#user-api-summary)
    * [Model Initialisation and Configuration](#model-initialisation-and-configuration)
    * [Transaction Layer Packet Generation](#transaction-layer-packet-generation)
    * [Data Link Layer Packet Generation](#data-link-layer-packet-generation)
    * [Ordered Set Generation](#ordered-set-generation)
    * [Internal Memory Access](#internal-memory-access)
    * [Internal Configuration Space Access](#internal-configuration-space-access)
    * [Receiving Data](#receiving-data)
* [Additionally Provided Functionality](#additionally-provided-functionality)
  * [C plus plus Support](#c-plus-plus-support)
  * [Running the Model as an Endpoint](#running-the-model-as-an-endpoint)
    * [Configuration Space Helper Structures](#configuration-space-helper-structures)
    * [Setting Up the Configuration Space](#setting-up-the-configuration-space)
  * [Model Limitations](#model-limitations)
    * [Endpoint Feature Limitations](#endpoint-feature-limitations)
    * [LTSSM Limitations](#ltssm-limitations)
    * [Model Verification](#model-verification)

## Introduction

[_pcievhost_](https://github.com/wyvernSemi/pcievhost) runs a PCIe C model on a [_VProc_](https://github.com/wyvernSemi/vproc) virtual processor and drives the lanes of a PCIe 1.1 or 2.0 link, up to 16 lanes. It gives you:

* an API to generate PCIe traffic on the link;
* helper code for _some_ LTSSM link training features (not strictly part of the model);
* a sparse memory, as target for MemRd and MemWr;
* as an endpoint, a configuration space for CfgRd and CfgWr, pre-loadable, with a read-only mask. No special bit types (write one to clear etc.).

Details: the [_pcievhost_ manual](https://github.com/wyvernSemi/pcievhost/blob/master/doc/pcieVHost.pdf).

## The HDL component

`pcieVHost.sv` (in `verilog/pcieVHost`) is the model. `pcieVHostPipex1.sv`, in the same directory, wraps it as one link port of PIPE TX and RX data. Our `pcieVHostPipexN.sv` (in `5.sim/models`) does the same for any number of lanes.

<div align=center>
<img width=800 src="images/pcievhost_module.png">
</div>

Clocks:

* `pcieclk` runs at the raw bit rate $\div$ 10: 250MHz (4000ps) at GEN1, 500MHz (2000ps) at GEN2. Its reset must come from the same clock domain.
* For PIPE widths of 16, 32 or 64, internal logic converts to and from the symbol stream on `pclk` = `pcieclk` / (width / 8). E.g. 64 bits: `pcieclk` / 8 = 31.25MHz (GEN1) or 62.5MHz (GEN2).

By default the model sends 8b10b encoded data. Software can switch it to unencoded, unscrambled data: bits 7:0 a byte, bit 8 the K flag. openPCIE needs this mode.

| Parameter | Default | Sets |
|---|---|---|
| `LinkWidth` | 16 | active lanes, from lane 0. Unused lane ports may be left open. `pcieVHostPipex1` hardwires 1 |
| `EndPoint` | 0 | non-zero enables endpoint features. openPCIE sets 1 |
| `NodeNum` | 8 | node number of the internal _VProc_; unique per _VProc_ in the simulation |
| `DataWidth` | | PIPE data port width |

### Special values on the line

One rule: the line never carries X or Z, on any simulator. Electrical idle is a driven value, and inside the model it reaches the core on its `ElecIdleIn` and `ElecIdleOut` ports. Upstream had Z on 4-state simulators, and those ports under Verilator only. Issue numbers are in the [5.sim README](../../README.md#simulation-issues).

| On | Value | Means |
|---|---|---|
| PIPE (`pcieVHostPipex1.sv`, `pcieVHostPipexN.sv`) | `` `NOSIG `` with the K flag, per symbol | electrical idle, both ways. `0xFF` with K has no 8b/10b code, so PCIe never sends it (4.2.1). Defined with the K codes in `pciexpress_header.svh` (our patch); our models take it from `models/pipe_sym_pkg.sv`, and `pcieVHostPipexN.sv` checks that the two agree |
| serial (`pcieVHost.sv`, `Serialiser.sv`) | constant 0 | electrical idle |
| | a symbol of ten equal bits | idle seen: symbol sync dropped, and the core told the partner is idle. No 8b/10b symbol runs past five bits; a symbol of 1s is idle too, as with the lane's polarity inverted |
| | K28.5 (`` `PCOMMA ``, `` `NCOMMA ``) | symbol sync; the symbol count restarts there (issue 49) |
| | an inverted TS1 or TS2 identifier (D21.5 `0xB5`, D26.5 `0xBA`) | lane polarity inverted (4.2.4.4): the lane inverts itself (issue 52) |

The same on Verilator, Icarus and XSIM.

## Link Traffic Display

The model prints link traffic to the simulation console. You choose the layers, the cycles it is on, and the link end. Colour separates downstream from upstream; turn it off for GUI consoles that can't show it.

Control it with `hex/ContDisps.hex`, below the run directory. Each line: a hex control value, then the <u>decimal</u> cycle it takes effect.

```
// Example ContDisps.hex file
// Copy to the appropriate test/hex directory, and edit as necessary.
//                      +8              +4              +2              +1
//  ,---> 11 - 8:    DispSwNoColour     DispSwEnIfEp   DispSwEnIfRc    DispSwTx
//  |,-->  7 - 4:    DispRawSym         DispPL         DispDL          DispTL
//  ||,->  3 - 0:    Unused             DispStop       DispFinish      DispAll
//  ||| ,-> Time (clock cycles, decimal)
//  ||| |
    370 000000000000
    002 009999999999
```

| Bit | Name | Effect |
|---|---|---|
| 11 | `DispSwNoColour` | 1 = no colour codes |
| 10, 9 | `DispSwEnIfEp`, `DispSwEnIfRc` | display at an endpoint / root complex end (lets other link displays coexist) |
| 8 | `DispSwTx` | show transmitted traffic too (default: received only) |
| 7 | `DispRawSym` | raw link symbols: a lot of output, hard to read |
| 6, 5, 4 | `DispPL`, `DispDL`, `DispTL` | physical, data link, transaction layer. Higher layers indent when lower ones are on |
| 2 | `DispStop` | stop the simulation (no exit) |
| 1 | `DispFinish` | finish the simulation |
| 0 | `DispAll` | all layers, whatever the other bits |

Output for the example file above:

<div align=center>
<img  width=750 src="images/pcie_disp_terminal.png" style="box-shadow: 5px 5px 5px gray;">
</div>

More in the [_pcievhost_ manual](https://github.com/wyvernSemi/pcievhost/blob/master/doc/pcieVHost.pdf).

## The PCIe C Model

### Internal Architecture

<div align=center>
<img width=1000 src="images/pcievhost_architecture.png">
</div>

* **User code** (left): `VUserMain`<i>n</i> is the entry point and calls the [API](#user-api-summary). An optional callback (`VUserInput`) gets received packets the model does not handle, e.g. read completions.
* **Send path:** TLP and DLLP calls queue packets for `SendPacket`; manual ACKs and NAKs skip the queue. `SendPacket` hands them to the `codec`, which scrambles, encodes and drives the link.
* **Receive path:** the `codec` decodes and descrambles into `ExtractPhyInput`. Ordered sets go to `ProcessOs` (the API reads its event counts); TLPs and DLLPs go to `ProcessInput`.
* `ProcessInput` serves MemRd/MemWr from the internal memory and, at an endpoint, CfgRd/CfgWr from the config space. It queues valid completions for reads itself. Every other TLP goes to the user callback.

### User API Summary

This is a summary of a large API; the [_pcievhost_ manual](https://github.com/wyvernSemi/pcievhost/blob/master/doc/pcieVHost.pdf) has the arguments. These are the C functions; the C++ class `pcieModelClass` wraps them as methods ([C plus plus Support](#c-plus-plus-support)).

#### Model Initialisation and Configuration

| **API Function**      | **Description** |
|-----------------------|-------------|
| `InitialisePcie`      | Initialise the model |
| `ConfigurePcie`       | Configure the model |
| `ConfigurePcieLtssm`  | Configure the model's LTSSM (part of the external LTSSM model, API defined in `ltssm.h`)|
| `PcieSeed`            | Seed internal random number generator |

* `InitialisePcie` comes first. It takes an optional callback for all unhandled received packets, and an optional user pointer passed back to the callback untouched (e.g. for cross checking).
* `ConfigurePcie` and `ConfigurePcieLtssm` set one parameter per call: a type and, where needed, a value.
* `PcieSeed` seeds the generator behind `PcieRand`, which the API also exposes. `GetCycleCount` returns the model's cycle count.

##### Configuration via ConfigurePcie

`ConfigurePcie(type, value)`:

| **TYPE**                       |**VALUE**|**UNITS**| **Description**                                                              |
|--------------------------------|---------|---------|------------------------------------------------------------------------------|
| CONFIG_FC_HDR_RATE             |   yes   | cycles  | Rx Header consumption rate (default 4)                                       |
| CONFIG_FC_DATA_RATE            |   yes   | cycles  | Rx Data consumption rate (default 4)                                         |
| CONFIG_ENABLE_FC               |   no    |         | Enable auto flow control (default)                                           |
| CONFIG_DISABLE_FC              |   no    |         | Disable auto flow control                                                    |
| CONFIG_ENABLE_ACK              |   yes   | cycles  | Enable auto acknowledges with processing rate (default rate 1)               |
| CONFIG_DISABLE_ACK             |   no    |         | Disable auto acknowledges                                                    |
| CONFIG_ENABLE_MEM              |   no    |         | Enable internal memory (default)                                             |
| CONFIG_DISABLE_MEM             |   no    |         | Disable internal memory                                                      |
| CONFIG_ENABLE_SKIPS            |   yes   | cycles  | Enable regular Skip ordered sets, with interval (default interval 1180)      |
| CONFIG_DISABLE_SKIPS           |   no    |         | Disable regular Skip ordered sets                                            |
| CONFIG_DISABLE_SCRAMBLING      |   no    |         | Disable data scrambling                                                      |
| CONFIG_ENABLE_SCRAMBLING       |   no    |         | Enable data scrambling (default)                                             |
| CONFIG_DISABLE_8B10B           |   no    |         | Disable 8b10b encoding and decoding                                          |
| CONFIG_ENABLE_8B10B            |   no    |         | Enable 8b10b encoding and decoding (default)                                 |
| CONFIG_DISABLE_ECRC_CMPL       |   no    |         | Disable ECRC auto-generation on completions for requests with ECRCs          |
| CONFIG_ENABLE_ECRC_CMPL        |   no    |         | Enable ECRC auto-generation on completions for requests with ECRCs (default) |
| CONFIG_ENABLE_UR_CPL           |   no    |         | Enable auto unsupported request completions (default)                        |
| CONFIG_DISABLE_UR_CPL          |   no    |         | Disable auto unsupported request completions                                 |
| CONFIG_ENABLE_INTERNAL_MEM     |   no    |         | Enable internal memory (default)                                             |
| CONFIG_DISABLE_INTERNAL_MEM    |   no    |         | Disable internal memory (packets passed to user callback if registered)      |
| CONFIG_ENABLE_DISPLINK_COLOUR  |   no    |         | Enable colour formatting of link display output (default)                    |
| CONFIG_DISABLE_DISPLINK_COLOUR |   no    |         | Disable colour formatting of link display output                             |
| CONFIG_POST_HDR_CR†            |   yes   | credits | Initial advertised posted header credits (default 32)                        |
| CONFIG_POST_DATA_CR†           |   yes   | credits | Initial advertised posted data credits (default 1K)                          |
| CONFIG_NONPOST_HDR_CR†         |   yes   | credits | Initial advertised non-posted header credits (default 32)                    |
| CONFIG_NONPOST_DATA_CR†        |   yes   | credits | Initial advertised non-posted data credits (default 1)                       |
| CONFIG_CPL_HDR_CR†             |   yes   | credits | Initial advertised completion header credits (default ∞)                     |
| CONFIG_CPL_DATA_CR†            |   yes   | credits | Initial advertised non-posted data credits (default ∞)                       |
| CONFIG_CPL_DELAY_RATE†         |   yes   | cycles  | Auto completion delay rate (default 0)                                       |
| CONFIG_CPL_DELAY_SPREAD†       |   yes   | cycles  | Auto completion delay randomised spread (default 0)                          |

† Call immediately after `InitialisePcie()` to take effect from time 0

##### Configuration via ConfigurePcieLtssm

`ConfigurePcieLtssm(type, value)`:

| **TYPE**                            |**VALUE**| **UNITS** | **Description**                                                                           |
|-------------------------------------|---------|-----------|-------------------------------------------------------------------------------------------|
| CONFIG_LTSSM_LINKNUM††              |  yes    | integer   | Training sequence advertised link number (default 0)                                      |
| CONFIG_LTSSM_N_FTS††                |  yes    | integer   | Training sequence number of fast training sequences (default 255)                         |
| CONFIG_LTSSM_TS_CTL††               |  yes    | integer   | Five bit TS control field (default 0)                                                     |
| CONFIG_LTSSM_DETECT_QUIET_TO††      |  yes    | cycles    | Detect quiet timeout (default 1500/6M, depending if `LTSSM_ABBREVIATED` defined or not)   |
| CONFIG_LTSSM_POLL_ACTIVE_TO_COUNT†† |  yes    | cycles    | Polling active TX count (default 16/1024, depending if `LTSSM_ABBREVIATED` defined or not)|
| CONFIG_LTSSM_ENABLE_TESTS††         |  yes    | bit mask  | Enable LTSSM test exceptions (default 0)                                                  |
| CONFIG_LTSSM_FORCE_TESTS††          |  yes    | bit mask  | Force LTSSM test exceptions (default 0)                                                   |
| CONFIG_LTSSM_DISABLE_DISP_STATE††   |  yes    | integer   | Disable display of LTSSM state to console (default 0 = display state)                     |

†† Call before calling `InitLink()` to take effect in training sequences.

#### Transaction Layer Packet Generation

| **API Function**   | **Description**                                  |
|--------------------|--------------------------------------------------|
| `MemWrite`         | Generate a memory write transaction              |
| `MemRead`          | Generate a memory read transaction               |
| `Completion`       | Generate a Completion transaction                |
| `PartCompletion`   | Generate a partial completion transaction        |
| `CfgWrite`         | Generate a configuration space write transaction |
| `CfgRead`          | Generate a configuration space read transaction  |
| `IoWrite`          | Generate an IO write transaction                 |
| `IoRead`           | Generate an IO read transaction                  |
| `Message`          | Generate a message transaction                   |

* Each has a `...Digest` version (e.g. `MemWriteDigest`) with an argument to choose ECRC on or off. The plain versions generate the digest.
* `Completion` and `PartCompletion` also have delay versions, which send after the delay set at initialisation.
* `WaitForCompletion` and `WaitForCompletionN` wait for one or N completions.

#### Data Link Layer Packet Generation

| **API Function** | **Description**                      |
|------------------|--------------------------------------|
| `SendAck`          | Send an acknowledgement packet     |
| `SendNak`          | Send a not-acknowledgement packet  |
| `SendFC`           | Send a flow control packet         |
| `SendPM`           | Send a power management packet     |
| `SendVendor`       | Send a vendor specific packet      |

#### Ordered Set Generation

| **API Function** | **Description**                                          |
|------------------|----------------------------------------------------------|
| `SendIdle`       | Send idle symbols on all active lanes                    |
| `SendOs`         | Send an ordered set down all active lanes                |
| `SendTs`         | Send a training sequence ordered set on all active lanes |

`SendTs` fills in lane numbers unless asked for `PAD`. The model counts received training sequences per lane: read them with `ReadEventCount`, clear them with `ResetEventCount`. `GetTS` returns the last TS processed on a lane.

#### Internal Memory Access

| **API Function**    | **Description**                            |
|---------------------|--------------------------------------------|
| `WriteRamByte`      | Write a byte to internal memory            |
| `WriteRamHWord`     | Write a 16-bit half-word to internal memory |
| `WriteRamWord`      | Write a 32-bit word to internal memory     |
| `WriteRamDWord`     | Write a 64-bit double word to internal memory |
| `ReadRamByte`       | Read a byte from internal memory           |
| `ReadRamHWord`      | Read a 16-bit half-word from internal memory |
| `ReadRamWord`       | Read a 32-bit word from internal memory    |
| `ReadRamDWord`      | Read a 64-bit double word from internal memory |
| `WriteRamByteBlock` | Write a block of bytes to internal memory  |
| `ReadRamByteBlock`  | Read a block of bytes from internal memory |

The internal sparse memory covers the full 64-bit address space. It is the target of received MemWr and MemRd; these functions access it directly.

#### Internal Configuration Space Access

| **API Function**       | **Description**                                                                                      |
|------------------------|------------------------------------------------------------------------------------------------------|
| `WriteConfigSpace`     | Write a 32-bit value to the configuration space                                                      |
| `ReadConfigSpace`      | Read a 32-bit value from the configuration space                                                     |
| `WriteConfigSpaceMask` | Write a 32-bit mask value to the configuration space mask (bits set to 1 become read only over PCIe) |
| `ReadConfigSpaceMask`  | Read a 32-bit value from the configuration space mask                                                |

An endpoint has a 4096 x 32-bit configuration memory, blank by default, read and written by CfgRd and CfgWr. A shadow mask of the same size, all 0s by default, makes bits read-only: where a mask bit is 1, CfgWr over the link does not change the config bit.

#### Receiving Data

The registered callback gets every received packet the model did not handle itself, most often completions for memory or configuration reads. Arguments: the packet (`pPkt_t`), a status, and the user pointer, if one was registered.

`status` ORs together these flags (`PKT_STATUS_GOOD` is 0):

|**Status Value**       | **Packet type** |
|-----------------------|-----------------|
|PKT_STATUS_GOOD        | all             |
|PKT_STATUS_BAD_LCRC    | TLP and DLLP    |
|PKT_STATUS_BAD_ECRC    | TLP             |
|PKT_STATUS_UNSUPPORTED | TLP completion  |
|PKT_STATUS_NULLIFIED   | TLP             |

The packet structure:

```C
typedef struct  pkt_struct {
    pPkt_t      NextPkt;     // Pointer to next packet to be sent
    PktData_t   *data;       // Pointer to a raw data packet, terminated by -1
    int         seq;         // DLL sequence number for packet (-1 for DLLP)
    int         Retry;
    uint32_t    TimeStamp;
    uint32_t    ByteCount;
} sPkt_t;
```

| Field | Holds |
|---|---|
| `NextPkt` | ignore in the callback |
| `data` | the whole raw packet, one `PktData_t` per byte, ended by -1. `PktData_t` is not 8 bits wide: don't overlay it as `char` |
| `seq` | sequence number of the packet |
| `Retry` | retries already made for the packet |
| `TimeStamp` | cycle count when the packet was fully received |
| `ByteCount` | payload size in bytes, may be 0 |

Helper macros decode the raw data. `GET_TLP_PAYLOAD_PTR(pkt)` points at the payload, for 3- or 4-word headers; with `ByteCount` that gives the data. Some others:

|**Macro**             |**Description**                                                      |
|----------------------|---------------------------------------------------------------------|
| GET_TLP_TYPE(pkt)    | Get the type code of the TLP packet (as defined in `pci_express.h`) |
| GET_TLP_ADDRESS(pkt) | Get a TLP's address as `uint64_t`                                   |
| GET_TLP_FBE(pkt)     | Get a TLP packet's first byte enable value                          |
| GET_TLP_LBE(pkt)     | Get a TLP packet's last byte enable value                           |
| GET_TLP_RID(pkt)     | Get a TLP packet's requester ID                                     |
| GET_TLP_TAG(pkt)     | Get a TLP packet's tag value                                        |
| TLP_HAS_DIGEST(pkt)  | Flag if a TLP has an ECRC digest                                    |
| TLP_IS_POSTED(pkt)   | Flag if a TLP is a posted request                                   |

TLP types (`pci_express.h`):

| **TYPE**       | **Description**                                    |
|----------------|----------------------------------------------------|
|TL_MRD32        | A memory read request with a 32-bit address        |
|TL_MRD64        | A memory read request with a 64-bit address        |
|TL_MRDLCK32     | A locked memory read request with a 32-bit address |
|TL_MRDLCK64     | A locked memory read request with a 64-bit address |
|TL_MWR32        | A memory write with a 32-bit address               |
|TL_MWR64        | A memory write with a 64-bit address               |
|TL_IORD         | A memory I/O read request                          |
|TL_IOWR         | A memory I/O write                                 |
|TL_CFGRD0       | A Type 0 configuration space read request          |
|TL_CFGWR0       | A Type 0 configuration space write                 |
|TL_CFGRD1       | A Type 1 configuration space read request          |
|TL_CFGWR1       | A Type 1 configuration space write                 |
|TL_MSG          | A message with no payload                          |
|TL_MSGD         | A message with payload                             |
|TL_CPL          | A completion with no payload                       |
|TL_CPLD         | A completion with payload                          |
|TL_CPLLK        | A locked access completion with no payload         |
|TL_CPLDLK       | A locked access completion with payload            |

The callback <u>must</u> free each packet with `DISCARD_PACKET(pkt)`, usually as its last line. Copy out anything you keep first, e.g. into a queue reached through the user pointer.

A simple callback for a root complex:

```C
 void VUserInput_0(pPkt_t pkt, int status, void* usrptr)
{
    int idx;

    // Is a DLLP
    if (pkt->seq == DLLP_SEQ_ID)
    {
        DebugVPrint("---> VUserInput_0 received DLLP\n");
    }
    // A TLP
    else
    {
        DebugVPrint("---> VUserInput_0 received TLP sequence %d of %d bytes at %d\n",
                       pkt->seq, GET_TLP_LENGTH(pkt->data), pkt->TimeStamp);

        // If a completion, extract the TPL payload data and display
        if (pkt->ByteCount)
        {
            // Get a pointer to the start of the payload data
            pPktData_t payload = GET_TLP_PAYLOAD_PTR(pkt->data);

            // Display the data
            DebugVPrint("---> ");
            for (idx = 0; idx < pkt->ByteCount; idx++)
            {
                DebugVPrint("%02x ", payload[idx]);
                if ((idx % 16) == 15)
                {
                    DebugVPrint("\n---> ");
                }
            }

            if ((idx % 16) != 0)
            {
                DebugVPrint("\n");
            }
        }
    }

    // Once packet is finished with, the allocated space *must* be freed.
    // All input packets have their own memory space to avoid overwrites
    // with shared buffers.
    DISCARD_PACKET(pkt);
}
```

At an endpoint with the internal memory and auto-completions on (the default), the callback only sees unhandled packets, such as messages and I/O accesses; it may be left out.

## Additionally Provided Functionality

The model began as a root complex PCIe 1.1/2.0 traffic generator, with no LTSSM or DLL initialisation. <i><b>Partial</b></i> versions of both are now included, for demonstration and education:

| **API Function** | **Description** |
|------------------|-------------|
| `InitLink`       | Go through an LTSSM link training sequence (part of the separate LTSSM model, defined in `ltssm.h`) |
| `InitFC`         | Initialise DLL flow control credits |

* They cover an initialisation with no exception conditions.
* The upstream LTSSM trains once: no Recovery, no retrain at a higher generation (GEN1 to GEN2). openPCIE adds this ([LTSSM Limitations](#ltssm-limitations)).
* Define `LTSSM_ABBREVIATED` when compiling `ltssm.c` for a short power-up: `Polling.Active` TX count 1024 -> 16, `Detect.Quiet` timeout 12ms -> 1500 cycles.
* Hooks exist for the other LTSSM paths but are not implemented. The LTSSM is a guide and reference, not part of the model proper.
* The DLL initialisation is complete; it too is example code.

### C plus plus Support

`pcieModelClass` (declared in `pcieModelClass.h`, compiled into the library) maps the C functions one to one onto methods, with some defaulted arguments. The node number is set once, at construction. The LTSSM API (`ConfigurePcieLtssm()`, `InitLink()`, in `ltssm.c`/`ltssm.h`) is not part of the class.

```C++
class pcieModelClass
{
public:
               pcieModelClass       (const unsigned nodeIn) : node (nodeIn) {};

    // TLP generation
    pPktData_t memWrite             (const uint64_t addr, const PktData_t *data, const int length, const int tag,
                                     const uint32_t rid, const bool queue = false, const bool digest = false);

    pPktData_t memRead              (const uint64_t addr, const int length, const int tag, const uint32_t rid,
                                    const bool queue = false, const bool digest = false);

    pPktData_t completion           (const uint64_t addr, const PktData_t *data, const int status, const int fbe, const int lbe,
                                     const int length,  const int tag, const uint32_t cid, const uint32_t rid,
                                     const bool queue = false, const bool digest = false);

    pPktData_t partCompletion       (const uint64_t addr, const PktData_t *data, const int status, const int fbe, const int lbe,
                                     const int rlength, const int length, const int tag, const uint32_t cid, const uint32_t rid,
                                     const bool queue = false, const bool digest = false);

    pPktData_t cfgWrite             (const uint64_t addr, const PktData_t *data, const int length, const int tag,
                                    const uint32_t rid, const bool queue = false, const bool digest = false);

    pPktData_t cfgRead              (const uint64_t addr, const int length, const int tag, const uint32_t rid,
                                     const bool queue = false, const bool digest = false);

    pPktData_t ioWrite              (const uint64_t addr, const PktData_t *data, const int length, const int tag,
                                     const uint32_t rid, const bool queue = false, const bool digest = false);

    pPktData_t ioRead               (const uint64_t addr, const int length, const int tag, const uint32_t rid,
                                     const bool queue = false, const bool digest = false);

    pPktData_t message              (const int code, const PktData_t *data, const int length, const int tag, const uint32_t rid,
                                    const bool queue = false, const bool digest = false);


    // Flow control initialisation
    void       initFc               (void);

    // Queue flushing
    void       sendPacket           (void)

    // Dllps
    void       sendAck              (const int seq);
    void       sendNak              (const int seq);
    void       sendFC               (const int type,  const int vc,    const int hdrfc, const int datafc, const bool queue);
    void       sendPM               (const int type,  const bool queue = false);

    void       sendVendor           (const bool queue = false);
    void       sendVendor           (const int data,  const bool queue = false);

    // Physical layer Ordered sets etc.
    void       sendIdle             (const int ticks = 1);
    void       sendOs               (const int type);
    void       sendTs               (const int identifier, const int lane_num, const int link_num, const int n_fts,
                                     const int control, const bool is_gen2 = false);

    void       waitForCompletion    (const uint32_t count = 1);
    void       initialisePcie       (const callback_t    cb_func, void *usrptr);
    void       registerOsCallback   (const os_callback_t cb_func);
    uint32_t   getCycleCount        (void);
    void       configurePcie        (const config_t type, const int value = 0);

    // Physical layer event routines
    int        resetEventCount      (const int type);
    int        readEventCount       (const int type, uint32_t *ts_data);
    TS_t       getTS                (const int lane);

    // Miscellaneous support routines
    uint32_t   pcieRand             (void);
    void       pcieSeed             (const uint32_t seed);
    void       setTxEnabled         (void);
    void       setTxDisabled        (void);
    void       getPcieVersionStr    (char* sbuf, const int bufsize);

    // Memory access
    void       writeRamByteBlock    (const uint64_t addr, const PktData_t* const data, const int fbe, const int lbe,
                                     const int length);
    int        readRamByteBlock     (const uint64_t addr, PktData_t* const data, const int length);

    void       writeRamByte         (const uint64_t addr, const uint32_t data);
    void       writeRamHWord        (const uint64_t addr, const uint32_t data, const int little_endian = 0);
    void       writeRamWord         (const uint64_t addr, const uint32_t data, const int little_endian = 0);
    void       writeRamDWord        (const uint64_t addr, const uint64_t data, const int little_endian = 0);
    uint32_t   readRamByte          (const uint64_t addr);
    uint32_t   readRamHWord         (const uint64_t addr, const int little_endian = 0);
    uint32_t   readRamWord          (const uint64_t addr, const int little_endian = 0);
    uint64_t   readRamDWord         (const uint64_t addr, const int little_endian = 0);

    // Configuration space access
    void       writeConfigSpace     (const uint32_t addr, const uint32_t data);
    uint32_t   readConfigSpace      (const uint32_t addr);
    void       writeConfigSpaceMask (const uint32_t addr, const uint32_t data);
    uint32_t   readConfigSpaceMask  (const uint32_t addr);

private:
    unsigned node;
};
```

### Running the Model as an Endpoint

Endpoint features exist to give the root complex model a target: a configuration space, and automatic completions from the internal memory and the config space. An endpoint that only responds needs little code:

```C
#include <stdio.h>
#include <stdlib.h>
#include "pcieModelClass.h"
#include "ltssm.h"

static void VUserInput_1(pPkt_t pkt, int status, void* usrptr)
{
  /* Do stuff here with input */
}

static void ConfigureType0PcieCfg (pcieModelClass *pcie)
{
  /* Initialise the configuration space and mask here */
}

extern "C" void VUserMain1(int node)
{
    // Create a pcie model API object for this node 
    pcieModelClass *pcie = new pcieModelClass(node);

    // Initialise PCIe VHost, with input callback function and no user pointer.
    pcie->initialisePcie(VUserInput_1, NULL);

    // Configure the model for PIPE operation
    pcie->configurePcie(CONFIG_DISABLE_SCRAMBLING);
    pcie->configurePcie(CONFIG_DISABLE_8B10B);

    // Configure LTSSM for full timings (longer link training simulation time)
    ConfigurePcieLtssm(CONFIG_LTSSM_DETECT_QUIET_TO,      6000000, node);
    ConfigurePcieLtssm(CONFIG_LTSSM_POLL_ACTIVE_TO_COUNT, 1024,    node);

    // Make sure the link is out of electrical idle
    VWrite(LINK_STATE, 0, 0, node);

    // Use node number as seed
    pcie->pcieSeed(node);

    // Construct an endpoint PCIe configuration space
    ConfigureType0PcieCfg(pcie);

    // Initialise the link for 16 lanes
    InitLink(16, node);

    // Initialise flow control
    pcie->initFc();

    // Send out idles forever
    while (true)
    {
        pcie->sendIdle(100);
    }
}
```

The program, in order:

1. initialises the model with the callback `VUserInput_1`;
2. overrides defaults, then configures the LTSSM;
3. takes the link out of electrical idle with a low-level _VProc_ call (`VWrite(LINK_STATE, ...)`);
4. seeds the random generator and builds the config space;
5. trains the link, then initialises flow control;
6. sends idles forever. The model replaces them with its responses (completions, ACKs, flow control DLLPs) as needed.

Unless the endpoint starts transactions itself, nothing more is needed.

#### Configuration Space Helper Structures

`pci_express.h` has structures for building a valid Type 0 configuration space, limited to the PCI compatible region and the minimal capabilities. Each structure has a matching union with an array of 32-bit words of the same size.

| **Structure**                     | **Union**                   | **Description**                         |
|-----------------------------------|-----------------------------|-----------------------------------------|
| `cfg_spc_type0_struct_t`          | `cfg_spc_type0_t`           | Type 0 configuration space              |
| `cfg_spc_pcie_caps_struct_t`      | `cfg_spc_pcie_caps_t`       | PCIe capabilities                       |
| `cfg_spc_msi_caps_struct_t`       | `cfg_spc_msi_caps_t`        | Message Signalled Interrupt capabilities |
| `cfg_spc_pwr_mgmnt_caps_struct_t` | `cfg_spc_pwr_mgmnt_caps_t`  | Power Management capabilities           |

Fill in the fields, then pass the words to `WriteConfigSpace`.

#### Setting Up the Configuration Space

By default the config space is a plain 4K x 32-bit memory, all 0: CfgWr writes every bit, CfgRd reads it back. To model a real one, fill it in `ConfigureType0PcieCfg`, left empty above. Below: a <u>minimal</u> Type 0 space for a proprietary network card, no PCIe extended capabilities. Background: [PCIe Primer](https://drive.google.com/file/d/1CECftcznLwcKDADtjpHhW13-IBHTZVXx), Part 4.

```C
static void ConfigureType0PcieCfg (pcieModelClass *pcie)
{
    unsigned        next_cap_ptr = 0;
    
    // -------------------------------------
    // PCI compatible header
    // -------------------------------------
    
    cfg_spc_type0_t     type0;
    cfg_spc_type0_t     type0_mask;
    cfg_spc_pcie_caps_t pcie_caps;
    cfg_spc_pcie_caps_t pcie_caps_mask;
    cfg_spc_msi_caps_t  msi_caps;
    cfg_spc_msi_caps_t  msi_caps_mask;
    pwr_mgmnt_caps_t    pwr_mgmnt_caps;
    pwr_mgmnt_caps_t    pwr_mgmnt_caps_mask;

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PCI_HDR_SIZE_BYTES/4); idx++)
    {
        type0.words[idx]      = 0x00000000;
        type0_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PCI_HDR_SIZE_BYTES;

    // Construct PCI compatible structure value, and set writable bits where appropriate
    type0.type0_struct.vendor_id               = 0x14fc;
    type0.type0_struct.device_id               = 0x0002;
    type0.type0_struct.command                 = 0x0006;     type0_mask.type0_struct.command = 0xfab8;
    type0.type0_struct.status                  = 0x0010;
    type0.type0_struct.revision_id             = 0x01;
    type0.type0_struct.prog_if                 = 0x00;       // don't care
    type0.type0_struct.subclass                = 0x80;       // other
    type0.type0_struct.class_code              = 0x02;       // network controller
    type0.type0_struct.cache_line_size         = 0x00;       type0_mask.type0_struct.cache_line_size = 0x00; 
    type0.type0_struct.bar[0]                  = 0x00000008; type0_mask.type0_struct.bar[0]          = 0x00000fff; // 32-bit, prefetchable, 4K
    type0.type0_struct.bar[1]                  = 0x00000008; type0_mask.type0_struct.bar[1]          = 0x000003ff; // 32-bit, prefetchable, 1K
    type0.type0_struct.bar[2]                  = 0x00000000;
    type0.type0_struct.bar[3]                  = 0x00000000;
    type0.type0_struct.bar[4]                  = 0x00000000;
    type0.type0_struct.bar[5]                  = 0x00000000;
    type0.type0_struct.expansion_rom_base_addr = 0x00000000; type0_mask.type0_struct.expansion_rom_base_addr = 0x000007fe;
    type0.type0_struct.capabilities_ptr        = next_cap_ptr;

    // Update config space and mask with values
    for (int idx = 0; idx < (CFG_PCI_HDR_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PCI_HDR_SIZE_BYTES + idx*4, type0.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PCI_HDR_SIZE_BYTES + idx*4, type0_mask.words[idx]);
    }
    
    // -------------------------------------
    // PCIe capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PCIE_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie_caps.words[idx]      = 0x00000000;
        pcie_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PCIE_CAPS_SIZE_BYTES;

    pcie_caps.pcie_caps_struct.cap_id         = 0x10;
    pcie_caps.pcie_caps_struct.next_cap_ptr   = next_cap_ptr;
    pcie_caps.pcie_caps_struct.device_caps    = 0x00000001;   // max payload = 256 bytes
    pcie_caps.pcie_caps_struct.device_control = 0x2810;       pcie_caps_mask.pcie_caps_struct.device_control = 0x0000;
    pcie_caps.pcie_caps_struct.link_caps      = 0x0003fc12;   
    pcie_caps.pcie_caps_struct.link_control   = 0x0000;       pcie_caps_mask.pcie_caps_struct.link_control   = 0xf004;
    pcie_caps.pcie_caps_struct.link_status    = 0x0091;       
    pcie_caps.pcie_caps_struct.link_control2  = 0x0002;       pcie_caps_mask.pcie_caps_struct.link_control2  = 0xe06f;

    for (int idx = 0; idx < (CFG_PCIE_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + idx*4, pcie_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PCIE_CAPS_SIZE_BYTES + idx*4, pcie_caps_mask.words[idx]);
    }
    
    // -------------------------------------
    // MSI capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_MSI_CAPS_SIZE_BYTES/4); idx++)
    {
        msi_caps.words[idx]      = 0x00000000;
        msi_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_MSI_CAPS_SIZE_BYTES;

    msi_caps.msi_caps_struct.cap_id           = 0x05;
    msi_caps.msi_caps_struct.next_cap_ptr     = next_cap_ptr;
    msi_caps.msi_caps_struct.mess_control     = 0x0080;     msi_caps_mask.msi_caps_struct.mess_control = 0xff8e;
    msi_caps.msi_caps_struct.mess_addr_lo     = 0x00000000; msi_caps_mask.msi_caps_struct.mess_addr_lo = 0x00000003;
    msi_caps.msi_caps_struct.mess_addr_hi     = 0x00000000; msi_caps_mask.msi_caps_struct.mess_addr_hi = 0x00000000;
    msi_caps.msi_caps_struct.mess_data        = 0x0000;     msi_caps_mask.msi_caps_struct.mess_data    = 0x0000;
    msi_caps.msi_caps_struct.mask             = 0x00000000; msi_caps_mask.msi_caps_struct.mask         = 0x00000000;
    msi_caps.msi_caps_struct.pending          = 0x00000000;

    for (int idx = 0; idx < (CFG_MSI_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_MSI_CAPS_SIZE_BYTES + idx*4, msi_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_MSI_CAPS_SIZE_BYTES + idx*4, msi_caps_mask.words[idx]);
    }

    // -------------------------------------
    // Power managment capability
    // -------------------------------------

    // Default to all zeros and read-only
    for (int idx = 0; idx < (CFG_PWR_MGMT_CAPS_SIZE_BYTES/4); idx++)
    {
        pwr_mgmnt_caps.words[idx]      = 0x00000000;
        pwr_mgmnt_caps_mask.words[idx] = 0xffffffff;
    }

    next_cap_ptr += CFG_PWR_MGMT_CAPS_SIZE_BYTES;

    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.cap_id                        = 0x01;
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.next_cap_ptr                  = 0x00; // last capability
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.pwr_mgmnt_caps                = 0x0003;
    pwr_mgmnt_caps.pwr_mgmnt_caps_struct.pwr_mgmnt_control_status      = 0x0008;
    pwr_mgmnt_caps_mask.pwr_mgmnt_caps_struct.pwr_mgmnt_control_status = 0xe0fc;

    for (int idx = 0; idx < (CFG_PWR_MGMT_CAPS_SIZE_BYTES/4); idx++)
    {
        pcie->writeConfigSpace     (next_cap_ptr - CFG_PWR_MGMT_CAPS_SIZE_BYTES + idx*4, pwr_mgmnt_caps.words[idx]);
        pcie->writeConfigSpaceMask (next_cap_ptr - CFG_PWR_MGMT_CAPS_SIZE_BYTES + idx*4, pwr_mgmnt_caps_mask.words[idx]);
    }
}
```

### Model Limitations

_pcievhost_ is a traffic pattern generator, first built to drive an endpoint design before a commercial sign-off VIP was available. The API generates any mix of PHY, DLL and TLP patterns. It is not a root complex or endpoint implementation: user code must sequence the patterns into valid traffic. Extra functions exist _in a limited capacity_.

#### Endpoint Feature Limitations

* Targets: internal memory for MemRd/MemWr, configuration space for CfgRd/CfgWr.
* The config space is blank until the user program fills it ([Setting Up the Configuration Space](#setting-up-the-configuration-space)).
* A mask makes bits read-only. No other register bit types, such as write one to clear.

#### LTSSM Limitations

The LTSSM in `ltssm.c` is an example, not part of the model. It goes from electrical idle to L0 at a given speed. Upstream it does not:

  * Support any exception, error or timeout routes through the LTSSM
  * Support switching speeds through Recovery and retrain automatically
  * Switch to lower power states
  * Switch to directed states such as Disabled, Loopback or Hot Reset
  * Auto reverse lanes
  * Auto invert lane polarity
  * Auto limit link width

Hooks for all of these are in the code. Lane width is a parameter, fixed for the simulation. GEN1 to GEN2 switching also needs the clocks switched at the right LTSSM sub-state.

**openPCIE:** our `ltssm.c` adds the speed change through Recovery, both ways, and `LinkIdle()`/`ChangeLinkSpeed()` to follow or start one from L0. It also adds the Configuration and Recovery timeouts, lane reversal (`SetLaneReversal()`) and lane polarity inversion ([issues 52, 59-63](../../README.md#simulation-issues)). Here the PIPE PHY model switches the clocks, following the RC's PIPE rate. See [the Gen2 speed change](../../README.md#example-the-gen2-speed-change).

#### Model Verification

First deployment: driving an endpoint as 16 lanes GEN1 or 8 lanes GEN2; almost all testing is at these widths and speeds. Other widths are implemented and tested with two _pcieVHost_ modules back to back, one RC, one EP.
