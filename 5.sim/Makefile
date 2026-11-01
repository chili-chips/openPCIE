###################################################################
# Makefile for virtual processor openpcie top level test bench
# code in Vivado xsim
#
# Copyright (C) 2025 Chili.CHIPS
#
###################################################################

# --------------------------------------------
# Command line modifiable variables
# --------------------------------------------

# What is always there is the endpoint model on node 1, plus the LTSSM helper
# it calls for link training. The root complex is the DUT (RC=hm), or the
# Artix-7 RC model on node 2 (RC=model, below). Node 0 is the SOC's CPU, and
# which model fills it is the CPU variable below.
USER_C           = ltssm.c
USRCODEDIR       = $(CURDIR)/usercode
PCIE_C           = VUserMain1.cpp $(if $(filter switched,$(VARIANT)),pcie_switch.cpp) $(if $(filter model,$(RC)),VUserMain2.cpp a7rc_check.cpp)
PCIECODEDIR      = $(CURDIR)/usercode
OPTFLAG         ?= -O2 -g
VPROC_PLI        = VProc.so
USRSIMOPTS       =
# BUILD (DEFAULT or ISS) follows CPU; not a command-line option
override BUILD   = DEFAULT

# --------------------------------------------
# The RC
# --------------------------------------------
# RC=hm (default): the RC_opensource DUT, with the PCIE_2_1 hard macro (tb.sv).
# RC=model: the Artix-7 RC model, pcieVHost on node 2 (usercode/VUserMain2.cpp),
# against the same endpoint, without the RTL (tb_rc.sv), x1 or x4.
# IFACE (RC=model): pipe (default), the PIPE as in tb.sv; serial, the serial
# line, both sides upstream's PcieVhostSerial with 8b/10b and scrambling
# (tb_rc_serial.sv).
# A7RC (RC=model): 0 (default), the tester: the lab RC's sequence, then what
# its firmware never sends but the spec allows (8 writes and 8 reads to BAR0
# back to back, checked); 1, literal: only what the lab RC sends. Both match
# the hard macro up to the firmware's result.
# SEED (RC=model): the seed of the tester's random stimulus (default 1); read
# at run time (A7RC_SEED), so a new seed needs no rebuild. make regress
# SEEDS=N runs seeds 1 to N, REGRESS_JOBS at a time, each in obj/regress/<seed>/
# with the packet display off (REGRESS_QUIET=0: as hex/ContDisps.hex), a time
# limit of REGRESS_S seconds, and again (up to REGRESS_TRIES times) when it
# hits the limit with no result (tools/regress.sh).
RC                ?= hm
ifdef RC_LINE
  $(error RC_LINE is now IFACE=pipe|serial)
endif
IFACE             ?= pipe
A7RC              ?= 0
SEED              ?= 1
SEEDS             ?= 10
REGRESS_S         ?= 900
REGRESS_TRIES     ?= 3
REGRESS_JOBS      ?= 1
REGRESS_QUIET     ?= 1
REGRESS_DUMP      ?= none
# make mutants: the checker against broken endpoints, MUTANT_JOBS at a time,
# each in obj/mutants/<name>/; each must fail the rule it breaks (tools/mutants.py)
MUTANT_JOBS       ?= 3
# make wavecrux RC=model SEED=N: that seed again with its waveform (DUMP_VCD, then
# the FST), opened in WaveCrux at the checker's first violation (tools/wavecrux.py;
# WaveCrux runs with --wcp-port 54321). WCX_OPEN=0: the dump only.
# REGRESS_DUMP=fail: after make regress, every failing seed dumped that way
WCX_OPEN          ?= 1
ifneq ($(filter regress,$(MAKECMDGOALS)),)
  ifneq ($(RC),model)
    $(error make regress needs RC=model)
  endif
endif
ifeq ($(filter hm model,$(RC)),)
  $(error RC must be hm or model (got "$(RC)"))
endif
ifeq ($(filter 0 1,$(A7RC)),)
  $(error A7RC must be 0 or 1 (got "$(A7RC)"))
endif
ifeq ($(A7RC)$(RC),1hm)
  $(error A7RC=1 needs RC=model)
endif

# --------------------------------------------
# Link: width and rate
# --------------------------------------------
# SIM_LANES: 1 (default) or 4.
# SIM_GEN: 2 lets both sides advertise Gen2, and the RC changes the link to
# Gen2 after L0; 1 pins the RC to Gen1 (SIM_GEN1_ONLY) and the endpoint model
# advertises Gen1. The default follows the bitstreams: Gen2 at x1, Gen1 at x4.
# It goes to link_pkg.sv as PCIE_GEN (xvlog.defines), which sets the user clock.
# SIM_RC_GEN (RC=model): the rates the RC model advertises, 1 or 2 (default
# SIM_GEN); SIM_RC_GEN=1 SIM_GEN=2 is a Gen1 RC and a Gen2 endpoint.
# SIM_SPEED_TEST=1 (SIM_GEN=2): the endpoint then changes the link down to
# Gen1 and back up (a Gen1 RC keeps it at Gen1).
# SIM_FAULT: the endpoint misbehaves, for the RC firmware's error paths:
# crs, nocpl, late or linkdown (usercode/VUserMain1.cpp).
SIM_LANES         ?= 1
SIM_GEN           ?= $(if $(filter 4,$(SIM_LANES)),1,2)
SIM_RC_GEN        ?= $(SIM_GEN)
# SIM_POLARITY=1 (RC=model IFACE=serial): both lines with D+ and D- swapped;
# each receiver must invert its lane in Polling (4.2.4.4)
SIM_POLARITY      ?= 0
# SIM_LANE_REVERSE=1 (x4): the lanes wired reversed, lane l to lane 3-l, both
# ways (4.2.4.9.1); SIM_LANE_REV: the port that may reverse its lanes, none, ep,
# rc (RC=model) or both
SIM_LANE_REVERSE  ?= 0
SIM_LANE_REV      ?= ep
SIM_SPEED_TEST    ?= 0
SIM_FAULT         ?= none
# HM_PROBE=1 (VARIANT=direct, x1): the endpoint measures the Data Link behaviour
# of the hard macro (RC=hm) or of the RC model (RC=model) under faults
# (usercode/hm_probe.h); RC=hm: on more firmware traffic (3.sw: -DHM_PROBE)
HM_PROBE          ?= 0
ifeq ($(filter 1 2,$(SIM_GEN)),)
  $(error SIM_GEN must be 1 or 2 (got "$(SIM_GEN)"))
endif
ifeq ($(filter 1 2,$(SIM_RC_GEN)),)
  $(error SIM_RC_GEN must be 1 or 2 (got "$(SIM_RC_GEN)"))
endif
ifneq ($(SIM_RC_GEN),$(SIM_GEN))
  ifneq ($(RC),model)
    $(error SIM_RC_GEN needs RC=model: the hard macro advertises SIM_GEN)
  endif
endif
ifeq ($(filter 0 1,$(SIM_POLARITY)),)
  $(error SIM_POLARITY must be 0 or 1 (got "$(SIM_POLARITY)"))
endif
ifeq ($(SIM_POLARITY)$(IFACE),1pipe)
  $(error SIM_POLARITY=1 needs IFACE=serial: on a PIPE the PHY inverts)
endif
ifeq ($(filter 0 1,$(SIM_LANE_REVERSE)),)
  $(error SIM_LANE_REVERSE must be 0 or 1 (got "$(SIM_LANE_REVERSE)"))
endif
ifeq ($(SIM_LANE_REVERSE)$(SIM_LANES),11)
  $(error SIM_LANE_REVERSE=1 needs SIM_LANES=4)
endif
ifeq ($(filter none ep rc both,$(SIM_LANE_REV)),)
  $(error SIM_LANE_REV must be none, ep, rc or both (got "$(SIM_LANE_REV)"))
endif
ifneq ($(filter rc both,$(SIM_LANE_REV)),)
  ifneq ($(RC),model)
    $(error SIM_LANE_REV=$(SIM_LANE_REV) needs RC=model: the hard macro does not reverse its lanes)
  endif
  ifeq ($(A7RC),1)
    $(error SIM_LANE_REV=$(SIM_LANE_REV) needs the tester: A7RC=1 does not reverse its lanes, as the hard macro)
  endif
endif
ifeq ($(SIM_LANE_REVERSE)$(SIM_LANE_REV),1none)
  $(error SIM_LANE_REVERSE=1 SIM_LANE_REV=none never trains: no port reverses its lanes)
endif
ifeq ($(filter 0 1,$(SIM_SPEED_TEST)),)
  $(error SIM_SPEED_TEST must be 0 or 1 (got "$(SIM_SPEED_TEST)"))
endif
ifeq ($(filter none crs nocpl late linkdown,$(SIM_FAULT)),)
  $(error SIM_FAULT must be none, crs, nocpl, late or linkdown (got "$(SIM_FAULT)"))
endif
ifeq ($(SIM_SPEED_TEST)$(filter none,$(SIM_FAULT)),1)
  $(error SIM_SPEED_TEST=1 and SIM_FAULT go one at a time)
endif
ifeq ($(SIM_SPEED_TEST)$(SIM_GEN),11)
  $(error SIM_SPEED_TEST=1 needs SIM_GEN=2: the endpoint waits for a Gen2 link)
endif
ifeq ($(filter pipe serial,$(IFACE)),)
  $(error IFACE must be pipe or serial (got "$(IFACE)"))
endif
ifeq ($(IFACE)$(RC),serialhm)
  $(error IFACE=serial needs RC=model)
endif
ifeq ($(RC),model)
  ifneq ($(or $(VARIANT),direct),direct)
    $(error RC=model is VARIANT=direct for now)
  endif
endif

# --------------------------------------------
# CPU model for the SOC (VProc node 0)
# --------------------------------------------
#   rtl   the real RTL core (CORE below), executing the compiled firmware.hex
#         out of the SOC's instruction RAM. This is the configuration that goes
#         into the bitstream, so it is both the default and the reference.
#   vproc VProc running the 3.sw firmware itself, compiled for the host
#         (usercode/VUserMain0.cpp calls its main). It drives the very same
#         CSR bus. No RISC-V involved at all.
#   iss   VProc running Simon's rv32 instruction set simulator on the real
#         firmware.elf -- real RISC-V instructions, interpreted rather than
#         executed by RTL.
#
# The DUT is identical in all three; only the module plugged into the CPU
# socket of riscv_pcie_soc.sv changes. See models/soc_cpu.VPROC.native.sv.
CPU             ?= rtl

# The RISC-V core: edubos5 (2-stage), edubos5_3stg or picorv32. The default
# follows the bitstreams: edubos5_3stg at x4 Gen2, edubos5 otherwise.
# CPU=rtl builds that core; CPU=iss uses its timing model (-V in vusermain.cfg).
CORE_DEF         = $(if $(and $(filter 4,$(SIM_LANES)),$(filter 2,$(SIM_GEN))),edubos5_3stg,edubos5)
CORE            ?= $(CORE_DEF)
ifeq ("$(CORE)", "edubos5")
  ISS_CORE       = EDUBOS5STG2
else ifeq ("$(CORE)", "edubos5_3stg")
  ISS_CORE       = EDUBOS5STG3
  CORE_DEFINE    = --define SOC_EDUBOS5_3STG
else ifeq ("$(CORE)", "picorv32")
  ISS_CORE       = PICORV32
  CORE_DEFINE    = --define SOC_CPU_PICORV32
else
  $(error CORE must be one of: edubos5, edubos5_3stg, picorv32  (got "$(CORE)"))
endif

ifeq ("$(CPU)", "vproc")
  USER_C        += VUserMain0.cpp
  FW_HOST_OBJS   = $(VOBJDIR)/fw_pcie.o $(VOBJDIR)/fw_main.o
  CPUDEFINE      = --define SOC_CPU_VPROC
else ifeq ("$(CPU)", "iss")
  override BUILD = ISS
  CPUDEFINE      = --define SOC_CPU_VPROC
else ifeq ("$(CPU)", "rtl")
  CPUDEFINE      = $(CORE_DEFINE)
else
  $(error CPU must be one of: rtl, vproc, iss  (got "$(CPU)"))
endif

# COUNTERS=1 (default): the CPU has its cycle counter, and the firmware times
# its waits with it (rdcycle); 0: neither (SOC_NO_COUNTERS), the waits count
# loops. Both come from this one switch, so they always match
COUNTERS        ?= 1
ifeq ($(filter 0 1,$(COUNTERS)),)
  $(error COUNTERS must be 0 or 1 (got "$(COUNTERS)"))
endif
ifeq ($(COUNTERS),0)
  CPUDEFINE     += --define SOC_NO_COUNTERS
endif

# DUMP_SOC_CPU (the CPU in WaveCrux, tb.sv) and SOC_IPC (the IPC count, tb.sv)
# need the core's RVFI outputs
ifneq ($(findstring DUMP_SOC_CPU,$(USRSIMOPTS))$(findstring SOC_IPC,$(USRSIMOPTS)),)
  CPUDEFINE     += --define RISCV_FORMAL
endif

# --------------------------------------------
# Global exported environment variables
# --------------------------------------------

HW_SRC          := $(CURDIR)/../2.rtl
BLD_DIR         := $(CURDIR)/../4.build
SIM_DIR         := $(CURDIR)/../5.sim
TB_NAME         := tb

export BLD_DIR HW_SRC SIM_DIR TB_NAME

# --------------------------------------------
# VProc C/C++ variables
# --------------------------------------------

# Location of VProc and memory model libraries
COSIMDIR         = $(CURDIR)/models/cosim
PCIEDIR          = $(CURDIR)/models/pcievhost

# --------------------------------------------
# RV32 ISS variables
# --------------------------------------------
#
# If BUILD is ISS, then override user source code list and directory
# and define extra flags
#
ifeq ("$(BUILD)", "ISS")
  USER_C         += VUserMain0.cpp mem_vproc_api.cpp uart.cpp rv32_cache.cpp vuserutils.cpp
  USRCODEDIR     = $(CURDIR)/models/rv32/usercode

  RV32DIR        = $(CURDIR)/models/rv32
  RV32SRC        = $(RV32DIR)/src
  RV32INCLOPTS   = -I$(RV32SRC)
  # the ISS library is built here from the rv32 sources (upstream iss/src)
  RV32OBJDIR     = $(CURDIR)/obj/rv32
  RV32LIB        = $(RV32OBJDIR)/librv32.a
  RV32LDOPTS     = $(RV32LIB)
  RV32_CPP       = rv32i_cpu_elf rv32_cpu_gdb rv32i_cpu rv32csr_cpu rv32m_cpu rv32a_cpu \
                   rv32f_cpu rv32d_cpu rv32c_cpu rv32zba_cpu rv32zbb_cpu rv32zbs_cpu rv32zbc_cpu
  RV32OBJS       = $(addprefix $(RV32OBJDIR)/, $(RV32_CPP:=.o))
  # options come from iss.cfg: vusermain.cfg with CORE's timing model
  # COUNTERS=1: the core reads its counters, so the ISS's RV32I filter lets them through
  ISS_DEFS       = -DCFGFILENAME=\"iss.cfg\" $(if $(filter 1,$(COUNTERS)),-DCORE_COUNTERS)
  RUN_CFG        = iss.cfg
endif

# --------------------------------------------
# VProc user code build variables
# --------------------------------------------

# Compiler executables and standard flags
CC               = $(HOST_CC)
C++              = $(HOST_CXX)
CPPSTD           = -std=c++20
ARCHFLAG         = -m64 

# C/C++ include paths for VProc, memory model and user code
# $(BLD_DIR)/csr_build carries csr_regs.h, the firmware's register accessors
# (VProc bus cycles with CPU=vproc), and generated-files/ the PeakRDL
# register layout they use (csr.h, csr_top.h).
INCLPATHS        = -I$(USRCODEDIR) -I$(PCIECODEDIR) -I$(COSIMDIR)/include -I$(PCIEDIR)/include -I$(PCIEDIR)/ltssm -I$(BLD_DIR)/csr_build -I$(BLD_DIR)/csr_build/generated-files $(RV32INCLOPTS)
DEFS             = -DVPROC_SV -DVPROC -DEXCLUDE_LTSSM $(ISS_DEFS) \
                   $(if $(filter switched,$(VARIANT)),-DSIM_SWITCHED) \
                   -DEP_GEN=$(SIM_GEN) -DRC_GEN=$(SIM_RC_GEN) $(if $(filter 1,$(SIM_SPEED_TEST)),-DSPEED_TEST) \
                   $(if $(filter-out none,$(SIM_FAULT)),-DSIM_FAULT_$(shell echo $(SIM_FAULT) | tr a-z A-Z)) \
                   $(if $(filter serial,$(IFACE)),-DSIM_SERIAL) -DA7RC_LITERAL=$(A7RC) \
                   $(if $(filter 1,$(HM_PROBE)),-DHM_PROBE) \
                   -DLANE_REV_EP=$(if $(filter ep both,$(SIM_LANE_REV)),1,0) -DLANE_REV_RC=$(if $(filter rc both,$(SIM_LANE_REV)),1,0)

VOBJDIR          = $(CURDIR)/obj

# Separate C and C++ source files
USER_CPP_BASE    = $(notdir $(filter %cpp, $(USER_C) $(PCIE_C)))
USER_C_BASE      = $(notdir $(filter %c,   $(USER_C) $(PCIE_C)))

# Create list of object files (excluding any veriuser object)
VOBJS            = $(addprefix $(VOBJDIR)/,                   \
                    $(USER_C_BASE:%.c=%.o)                    \
                    $(USER_CPP_BASE:%.cpp=%.o))               \
                   $(FW_HOST_OBJS)

USERLIB          = libuser.a

COSIMLDOPT       = -lcosimlnx
PCIELDOPT        = -lpcielnx

# xsim loads Vivado's own, older libstdc++ first; link ours in statically so
# VProc.so does not depend on which one is loaded
FLAGSOS          = -shared -lpthread -lrt -rdynamic -static-libstdc++ -static-libgcc

# Flags for compiling/linking shared object
CFLAGS_SO        = $(ARCHFLAG)                         \
                   $(FLAGSOS) -lpthread                \
                   -Wl,-whole-archive                  \
                   -L$(CURDIR)      -luser             \
                   -Wl,-no-whole-archive               \
                   -L$(COSIMDIR)/lib $(COSIMLDOPT)     \
                   -L$(PCIEDIR)/lib  $(PCIELDOPT)      \
                   $(RV32LDOPTS)                       \
                   -o $(VPROC_PLI)

# --------------------------------------------
# Simulation variables
# --------------------------------------------


WORKDIR          = work

# Tool binaries. ../4.build/set-env.sh finds them and sets these from the
# environment (source it first); anything left unset falls back to the bare
# name on PATH. HOST_CC/HOST_CXX build the VProc and pcievhost C/C++ code.
HOST_CC         ?= gcc
HOST_CXX        ?= g++
HOST_AR         ?= ar

# Xilinx simulation libraries. The design instantiates PCIE_2_1 (secureip),
# plus MMCME2_ADV, BUFG, BUFGCTRL and IBUFDS_GTE2 (unisims). glbl carries the
# GSR/GTS nets those models expect, so it is elaborated as a second top.
# Time limit of a run, in us. A run ends 10 us after the firmware reports its
# result; the limit only stops one that never does. Firmware built without
# SIM=1 opens with wait_cycles(100000), tens of milliseconds of simulated time
# (about 37 ms on eduBOS5, more on picorv32), so it needs more:
#   make run RUN_US=40000
RUN_US          ?= 2000

XILINX_VIVADO   ?= /tools/Xilinx/2026.1/Vivado
GLBL             = $(XILINX_VIVADO)/data/verilog/src/glbl.v
XILLIBS          = -L unisims_ver -L unimacro_ver -L secureip

# The firmware is built here, on every build, with the short co-simulation
# start-up delay (SIM=1), into obj/fw -- not into 4.build/sw_build, whose
# firmware.hex stays the board build. VARIANT=switched builds the RC-switched
# firmware, and puts a switch with endpoints behind the link partner
# (usercode/pcie_switch.cpp). riscv_pcie_soc.sv reads firmware.hex with a bare
# name, so a copy sits in this folder; the ISS (CPU=iss) runs
# obj/fw/firmware.elf.
VARIANT         ?= direct
ifeq ($(filter direct switched,$(VARIANT)),)
  $(error VARIANT must be direct or switched (got "$(VARIANT)"))
endif
ifeq ($(VARIANT)$(SIM_LANES),switched4)
  $(error VARIANT=switched is x1 only, as the board)
endif
# pcie_switch.cpp answers every request there, so the endpoint's config-space faults cannot act
ifeq ($(VARIANT)$(if $(filter crs nocpl linkdown,$(SIM_FAULT)),1),switched1)
  $(error SIM_FAULT=$(SIM_FAULT) needs VARIANT=direct: the switch model answers the requests it acts on)
endif
ifeq ($(HM_PROBE),1)
  ifneq ($(filter hm model,$(RC))$(VARIANT)$(SIM_FAULT)$(SIM_LANES)$(SIM_SPEED_TEST),$(RC)directnone10)
    $(error HM_PROBE=1 measures the RC (RC=hm or RC=model): VARIANT=direct, x1, no SIM_FAULT, no SIM_SPEED_TEST)
  endif
endif
FW_DIR           = $(VOBJDIR)/fw

# Flags for xsim
# SIM_FAST_TRAIN turns on the hard macro's shortened LTSSM training timers.
# It is a simulation-only parameter of PCIE_2_1 -- see pcie_core.sv.
# PCIE_LANES and PCIE_GEN go to xvlog through an options file (xvlog.defines); at 4
# lanes also PIPE_X4, which dumps the four-lane PIPE page (tb.sv).
TB_PRJ             = $(if $(filter model,$(RC)),$(if $(filter serial,$(IFACE)),tb_rc_serial.prj,tb_rc.prj),tb.prj)
ANALYSEFLAGS       = -i $(PCIEDIR)/verilog/headers --define VPROC_SV --define VPROC_BYTE_ENABLE --define SIM_FAST_TRAIN $(if $(filter 1,$(SIM_GEN)),--define SIM_GEN1_ONLY) $(if $(filter 1,$(SIM_POLARITY)),--define SIM_POLARITY) $(if $(filter 1,$(SIM_LANE_REVERSE)),--define SIM_LANE_REVERSE) --define SIM_PIPE_CODING -f xvlog.defines $(CPUDEFINE) $(USRSIMOPTS) --prj $(TB_PRJ)
ELABFLAGS          = -sv_lib $(VPROC_PLI) --timescale 1ps/1ps --debug typical $(XILLIBS) $(TB_NAME) glbl
# The snapshot xelab produces when two tops are elaborated is named
# "<top>#glbl". The hash has to be escaped or make treats the rest as a comment.
SIMFLAGS           = work.$(TB_NAME)\#work.glbl

XVLOG           ?= xvlog
XELAB           ?= xelab
SIMEXE          ?= xsim

#
# Formatting
#

SPC =

#======================================================
# BUILD RULES
#======================================================

.PHONY: all sysverilog sim run regress mutants wavecrux rungui gui fst help clean

all: sysverilog

# Rule to build object file temporary directory
$(VOBJDIR):
	@mkdir $(VOBJDIR)

# Rule to build user C sources
$(VOBJDIR)/%.o: $(USRCODEDIR)/%.c
	@$(CC) -c -fPIC -MMD -MP $(OPTFLAG) -Wno-write-strings $(DEFS) $(INCLPATHS) $< -o $@

# Rule to build user C++ sources
$(VOBJDIR)/%.o: $(USRCODEDIR)/%.cpp
	@$(C++) -c -fPIC -MMD -MP $(OPTFLAG) $(CPPSTD) -Wno-write-strings $(DEFS) $(INCLPATHS) $< -o $@

# Rule to build PCIe user C sources
$(VOBJDIR)/%.o: $(PCIECODEDIR)/%.c
	@$(CC) -c -fPIC -MMD -MP $(OPTFLAG) -Wno-write-strings $(DEFS) $(INCLPATHS) $< -o $@

# Rule to build PCIe user C++ sources
$(VOBJDIR)/%.o: $(PCIECODEDIR)/%.cpp
	@$(C++) -c -fPIC -MMD -MP $(OPTFLAG) $(CPPSTD) -Wno-write-strings $(DEFS) $(INCLPATHS) $< -o $@

# Rule to build PCIe LTSSM support model
$(VOBJDIR)/%.o: $(PCIEDIR)/ltssm/%.c
	@$(CC) -c -fPIC -MMD -MP $(OPTFLAG) -Wno-write-strings $(DEFS) $(INCLPATHS) $< -o $@

# CPU=vproc: the 3.sw firmware itself, compiled for the host as C++, with
# main() renamed fw_main() for usercode/VUserMain0.cpp to call. Under VPROC,
# csr_regs.h turns its register accesses into VProc bus cycles.
FW_SRC_DIR       = $(CURDIR)/../3.sw
FW_HOST_FLAGS    = -x c++ -Dmain=fw_main -Wno-volatile -I$(FW_SRC_DIR)/common -DCPL_TIMEOUT_US=2180 -DCRS_WAIT_US=374 -DSTARTUP_US=75
FW_HOST_DEPS     = $(FW_SRC_DIR)/common/pcie.h $(BLD_DIR)/csr_build/csr_regs.h \
                   $(BLD_DIR)/csr_build/generated-files/csr.h $(BLD_DIR)/csr_build/generated-files/csr_top.h

$(VOBJDIR)/fw_pcie.o: $(FW_SRC_DIR)/common/pcie.c $(FW_HOST_DEPS) | $(VOBJDIR)
	@$(C++) -c -fPIC -MMD -MP $(OPTFLAG) $(CPPSTD) $(FW_HOST_FLAGS) $(DEFS) $(INCLPATHS) $< -o $@

$(VOBJDIR)/fw_main.o: $(FW_SRC_DIR)/RC-$(VARIANT)/main.c $(FW_HOST_DEPS) | $(VOBJDIR)
	@$(C++) -c -fPIC -MMD -MP $(OPTFLAG) $(CPPSTD) $(FW_HOST_FLAGS) $(DEFS) $(INCLPATHS) $< -o $@

# The user code differs per CPU option (VUserMain0, the ISS and its defines),
# per VARIANT (the switch model, the firmware), per SIM_GEN/SIM_SPEED_TEST/SIM_FAULT
# (the endpoint), per RC, IFACE and A7RC (the RC model), per SIM_LANE_REV (both) and per COUNTERS (ISS_DEFS): a change of any throws the objects and the library away, so
# that the next build cannot mix them. obj/.cpu holds them for the last build.
CPU_STAMP = $(VOBJDIR)/.cpu
$(CPU_STAMP): FORCE | $(VOBJDIR)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(CPU) $(VARIANT) $(SIM_GEN) $(SIM_RC_GEN) $(SIM_POLARITY) $(SIM_SPEED_TEST) $(SIM_FAULT) $(RC) $(IFACE) $(A7RC) $(COUNTERS) $(HM_PROBE) $(SIM_LANE_REV)" ]; then \
	    rm -f $(VOBJDIR)/*.o $(USERLIB) $(VPROC_PLI); echo "$(CPU) $(VARIANT) $(SIM_GEN) $(SIM_RC_GEN) $(SIM_POLARITY) $(SIM_SPEED_TEST) $(SIM_FAULT) $(RC) $(IFACE) $(A7RC) $(COUNTERS) $(HM_PROBE) $(SIM_LANE_REV)" > $@; fi

# Rule to build library of user code
$(USERLIB): $(CPU_STAMP) $(VOBJS)
	@$(HOST_AR) cr $(USERLIB) $(VOBJS)

# Rules to build the rv32 ISS library (CPU=iss), with upstream's flags
$(RV32OBJDIR):
	@mkdir -p $@

$(RV32OBJDIR)/%.o: $(RV32SRC)/%.cpp $(wildcard $(RV32SRC)/*.h) | $(RV32OBJDIR)
	@$(C++) -c -fPIC $(ARCHFLAG) $(OPTFLAG) -I$(RV32SRC) -D_REENTRANT $< -o $@

$(RV32LIB): $(RV32OBJS)
	@rm -f $@
	@$(HOST_AR) cr $@ $(RV32OBJS)

# Rule to build VProc shared object
$(VPROC_PLI): $(USERLIB) $(RV32LIB)
	@$(C++) $(CFLAGS_SO) -o $@

#
# Compile simulation
#
# RC=model runs no firmware
sysverilog: $(VPROC_PLI) $(if $(filter hm,$(RC)),firmware.hex)
	@echo "--define PCIE_LANES=$(SIM_LANES) --define PCIE_GEN=$(SIM_GEN)$(if $(filter 4,$(SIM_LANES)), --define PIPE_X4)" > xvlog.defines
	@$(XVLOG) $(ANALYSEFLAGS)
	@$(XVLOG) $(GLBL)
	@$(XELAB) $(ELABFLAGS)

# Built and copied on every build, not by date: a tree on a network share can
# carry timestamps from the future, and a stale image would then look newer
# than its sources.
firmware.hex: FORCE
	@$(MAKE) --no-print-directory -C $(BLD_DIR)/sw_build SIM=1 VARIANT=$(VARIANT) COUNTERS=$(COUNTERS) HM_PROBE=$(HM_PROBE) \
	    LINK=x$(SIM_LANES)g$(SIM_GEN) OUT=$(FW_DIR)
	@cp -f $(FW_DIR)/firmware.hex $@


#======================================================
# EXECUTION RULES
#======================================================

# tb.sv reads the run length from this file at time 0
run_us.cfg: FORCE
	@echo $(RUN_US) > $@

iss.cfg: FORCE
	@sed -E -e "s/-V [A-Z0-9]+/-V $(ISS_CORE)/" -e "s|-t [^ ]*firmware\.elf|-t obj/fw/firmware.elf|" vusermain.cfg > $@

FORCE:

sim: sysverilog run_us.cfg $(RUN_CFG)
	@A7RC_SEED=$(SEED) $(SIMEXE) $(SIMFLAGS)

# A run with DUMP_VCD also converts tb.vcd into the tracked FST, named like
# the bitstreams: tb.x1g2.direct.fst, tb.x1g1.switched.fst, tb.x4g1.direct.fst
# (g is SIM_GEN, the fastest rate the link may train to; SIM_SPEED_TEST adds
# .speed, SIM_FAULT .fault-<name>). The conversion runs whatever xsim returns.
# xsim exits 0 even after $fatal, so a "Fatal:" line in xsim.log (a FAIL
# result, or the time limit) sets the exit code to 1. Those names are for CPU=rtl and the default CORE; any other CPU or core
# adds its name (tb.x1g2.direct.vproc.fst, not tracked), so it never replaces
# a tracked dump.
DUMP_TAG = $(if $(filter rtl,$(CPU)),$(filter-out $(CORE_DEF),$(CORE)),$(CPU))$(if $(filter 1,$(SIM_SPEED_TEST)),.speed)$(if $(filter-out none,$(SIM_FAULT)),.fault-$(SIM_FAULT))$(if $(filter 1,$(A7RC)),.a7rc)$(if $(filter-out 1,$(SEED)),.seed$(SEED))
DUMP_FST = tb.x$(SIM_LANES)g$(SIM_GEN).$(if $(filter model,$(RC)),model$(if $(filter serial,$(IFACE)),.serial),$(VARIANT))$(if $(filter 1,$(SIM_LANE_REVERSE)),.rev)$(if $(DUMP_TAG),.$(patsubst .%,%,$(DUMP_TAG))).fst
run: sysverilog run_us.cfg $(RUN_CFG)
	@rm -f xsim.log; A7RC_SEED=$(SEED) $(SIMEXE) -R $(SIMFLAGS); rc=$$?; grep -q '^Fatal:' xsim.log && rc=1; \
	$(if $(findstring DUMP_VCD,$(USRSIMOPTS)),bash tools/vcd2fst.sh tb.vcd $(DUMP_FST);) exit $$rc

# The tester over seeds 1 to SEEDS, each in obj/regress/<seed>/ with its own
# copy of the snapshot (xsim.dir), so seeds can run side by side
regress: sysverilog run_us.cfg $(RUN_CFG)
	@bash tools/regress.sh $(SEEDS) $(REGRESS_JOBS) $(REGRESS_S) $(REGRESS_TRIES) $(REGRESS_QUIET) \
	    $(VOBJDIR)/regress "xsim.dir $(notdir $(VPROC_PLI)) run_us.cfg $(RUN_CFG)" -- $(SIMEXE) -R $(SIMFLAGS); s=$$?; \
	$(if $(filter fail,$(REGRESS_DUMP)),for d in $(VOBJDIR)/regress/*/; do [ ! -e $$d/failed ] || \
	    $(MAKE) --no-print-directory wavecrux RC=model SEED=$$(basename $$d) WCX_OPEN=0; done;) exit $$s

# The checker against broken endpoints: each mutant must fail its rule
mutants:
	@python3 tools/mutants.py $(MUTANT_JOBS) $(VOBJDIR)/mutants

# One seed with its waveform, opened in WaveCrux at the first violation
wavecrux:
	@$(if $(filter model,$(RC)),,echo "wavecrux needs RC=model"; exit 1;) \
	rm -f xsim.log tb.vcd $(DUMP_FST); \
	$(MAKE) --no-print-directory run SEED=$(SEED) USRSIMOPTS="$(USRSIMOPTS) --define DUMP_VCD"; \
	if [ ! -f xsim.log ] || [ ! -f $(DUMP_FST) ]; then echo "seed $(SEED): no run or no waveform"; exit 1; fi; \
	cp xsim.log wavecrux.$(SEED).log; echo "seed $(SEED): $$(grep -a -m1 'RC MODEL RESULT' xsim.log), $(DUMP_FST)"; \
	$(if $(filter 1,$(WCX_OPEN)),python3 tools/wavecrux.py $(DUMP_FST) wavecrux.$(SEED).log,echo "  open it: make wavecrux RC=model SEED=$(SEED)")

fst:
	@bash tools/vcd2fst.sh tb.vcd $(DUMP_FST)

rungui: sysverilog run_us.cfg $(RUN_CFG)
	@A7RC_SEED=$(SEED) $(SIMEXE) -g --autoloadwcfg $(SIMFLAGS)

gui: rungui

help:
	@$(info make help          Display this message)
	@$(info make               Build C/C++ and HDL code without running simulation)
	@$(info make run           Build and run batch simulation)
	@$(info make regress       RC=model: run the tester over seeds 1 to SEEDS)
	@$(info make mutants       the checker against broken endpoints: each must fail its rule (MUTANT_JOBS at a time, default 3))
	@$(info make wavecrux      RC=model SEED=N: that seed with its waveform, in WaveCrux at the first violation (--wcp-port 54321))
	@$(info make rungui/gui    Build and run GUI simulation)
	@$(info make fst           Convert tb.vcd into tb.<link>.<variant>.fst, e.g. tb.x1g2.direct.fst (run does it when USRSIMOPTS has DUMP_VCD))
	@$(info make clean         clean previous build artefacts)
	@$(info )
	@$(info Command line configurable variables:)
	@$(info $(SPC) $(SPC) USER_C:       list of user source code files for VProc (default ltssm.c; CPU=vproc/iss add VUserMain0.cpp))
	@$(info $(SPC) $(SPC) PCIE_C:       list of user source code files for pcievhost modules (default VUserMain1.cpp, plus pcie_switch.cpp with VARIANT=switched))
	@$(info $(SPC) $(SPC) USRCODEDIR:   directory containing user source code (default $$(CURDIR)/usercode))
	@$(info $(SPC) $(SPC) OPTFLAG:      Optimisation flag for user VProc code (default -O2 -g))
	@$(info $(SPC) $(SPC) USRSIMOPTS:   additional simulator analysis flags, such as setting defines (default blank))
	@$(info $(SPC) $(SPC) CPU:          CPU model on VProc node 0 (default rtl))
	@$(info $(SPC) $(SPC) $(SPC)   rtl    the RTL core (CORE), executing firmware.hex)
	@$(info $(SPC) $(SPC) $(SPC)   vproc  the 3.sw firmware, compiled for the host (usercode/VUserMain0.cpp))
	@$(info $(SPC) $(SPC) $(SPC)   iss    the rv32 ISS, executing firmware.elf)
	@$(info $(SPC) $(SPC) CORE:         edubos5, edubos5_3stg or picorv32: the RTL core, or the ISS timing model (default edubos5, at x4 Gen2 edubos5_3stg))
	@$(info $(SPC) $(SPC) RC:           hm (the RC_opensource DUT, tb.sv) or model (the Artix-7 RC model, tb_rc.sv; direct) (default hm))
	@$(info $(SPC) $(SPC) IFACE:        with RC=model: pipe, or serial (the serial line, 8b/10b and scrambling on; tb_rc_serial.sv) (default pipe))
	@$(info $(SPC) $(SPC) A7RC:         1 with RC=model: literal, only what the lab RC sends; 0 (default): the tester, also back-to-back requests)
	@$(info $(SPC) $(SPC) SEED:         with RC=model: the seed of the tester's random stimulus, no rebuild (default 1))
	@$(info $(SPC) $(SPC) SEEDS:        make regress RC=model: seeds 1 to SEEDS, REGRESS_S seconds each (default 10, 900))
	@$(info $(SPC) $(SPC) REGRESS_JOBS: seeds run side by side (default 1); REGRESS_QUIET=0 keeps the packet display (default 1: off))
	@$(info $(SPC) $(SPC) REGRESS_DUMP: fail: dump every failing seed after make regress (default none))
	@$(info $(SPC) $(SPC) VARIANT:      direct (one endpoint) or switched (a switch, endpoints behind it); firmware built with SIM=1 into obj/fw (default direct))
	@$(info $(SPC) $(SPC) SIM_LANES:    link width, 1 or 4 (default 1))
	@$(info $(SPC) $(SPC) SIM_GEN:      fastest link rate, 1 or 2; 2 lets the RC change the link to Gen2 after L0 (default 2, at x4 1))
	@$(info $(SPC) $(SPC) SIM_RC_GEN:   with RC=model: the rates the RC advertises, 1 or 2; SIM_RC_GEN=1 SIM_GEN=2: a Gen1 RC, a Gen2 endpoint (default SIM_GEN))
	@$(info $(SPC) $(SPC) SIM_POLARITY: 1 with RC=model IFACE=serial: both lines with D+ and D- swapped; each receiver inverts its lane (default 0))
	@$(info $(SPC) $(SPC) SIM_LANE_REVERSE: 1 with SIM_LANES=4: the lanes wired reversed (default 0))
	@$(info $(SPC) $(SPC) SIM_LANE_REV: none, ep, rc or both: the port that may reverse its lanes; rc and both need RC=model (default ep))
	@$(info $(SPC) $(SPC) SIM_SPEED_TEST: 1 with SIM_GEN=2: the endpoint then changes the link to Gen1 and back to Gen2 (default 0))
	@$(info $(SPC) $(SPC) SIM_FAULT:    crs, nocpl, late or linkdown: the endpoint misbehaves, for the firmware's error paths (default none))
	@$(info $(SPC) $(SPC) RUN_US:       time limit in microseconds; a run ends 10 us after the firmware result (default $(RUN_US)))
	@$(info )

#======================================================
# CLEANING RULES
#======================================================

clean:
	@rm -rf $(WORKDIR) $(VOBJDIR) $(USERLIB) $(VPROC_PLI) xelab.* xvlog.* xsim.* *.wdb .Xil
	@rm -rf firmware.hex run_us.cfg iss.cfg *.jou *.log webtalk*

# -----------------------------------------------------------------------------
# Project:     openPCIE
# Description: NLnet-sponsored open-source implementation
# Version:     1.0
# Date:        May 24, 2024
# -----------------------------------------------------------------------------

# Header dependencies of the user objects (-MMD -MP)
-include $(wildcard $(VOBJDIR)/*.d)
