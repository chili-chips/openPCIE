//=====================================================================
//
// VProc.h                                            Date: 2004/12/13 
//
// Copyright (c) 2004-2025 Simon Southwell.
//
// This file is part of VProc.
//
// VProc is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// VProc is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with VProc. If not, see <http://www.gnu.org/licenses/>.
//
//=====================================================================
//
// Internal header for VProc definitions and data types
//
//=====================================================================

#ifndef _VPROC_H_
#define _VPROC_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>

#define VERSION_STRING         "VProc version 1.13.4. Copyright (c) 2004-2025 Simon Southwell."

#ifndef VP_MAX_NODES
#define VP_MAX_NODES            64
#endif

// Definitions for accesses
#define V_IDLE                  0
#define V_WRITE                 1
#define V_READ                  2

#define BURSTLENLOBIT           2
#define BEFIRSTLOBIT            14
#define BELASTLOBIT             18

// Error types
#define VP_USER_ERR             1

// Indexes for PLI function arguments
#define VPNODENUM_ARG           1
#define VPINTERRUPT_ARG         2
#define VPVALUE_ARG             2
#define VPDATAIN_ARG            2
#define VPDATAOUT_ARG           3
#define VPADDR_ARG              4
#define VPRW_ARG                5
#define VPTICKS_ARG             6

#define VPINDEX_ARG             2
#define VPACCESSIN_ARG          3   
#define VACCESSOUT_ARG          4

// A default string buffer size
#define DEFAULT_STR_BUF_SIZE    32

// Range of valid interrupt states
#define MIN_INTERRUPT_LEVEL     1
#define MAX_INTERRUPT_LEVEL     7

#define NUM_IRQ_QUEUE_BITS      10
#define MAX_QUEUED_VEC_IRQ      (1 << NUM_IRQ_QUEUE_BITS)
#define IRQ_QUEUE_INDEX_MASK    (MAX_QUEUED_VEC_IRQ - 1)
#define IRQ_QUEUE_COUNT_MASK    (MAX_QUEUED_VEC_IRQ*2 - 1)

// Bitfield structure for rw value of send_buf_t exchange structure
typedef struct {
    uint32_t write    : 1;
    uint32_t read     : 1;
    uint32_t burstlen : 12;
    uint32_t fbe      : 4;
    uint32_t lbe      : 4;
    uint32_t rsvd     : 10;
} rw_t;


// User thread to simulation exchange structure
typedef struct {
    unsigned int        addr;
    unsigned int        data_out;
    unsigned int        rw;
    void                *data_p;
    int                 ticks;
} send_buf_t, *psend_buf_t;

// Simulation to user thread exchange structure
typedef struct {
    unsigned int        data_in;
} rcv_buf_t, *prcv_buf_t;

// Shared object handle typedef
typedef void * handle_t;

// Callback pointer types
typedef int  (*pVUserIrqCB_t)    (int);
typedef int  (*pPyIrqCB_t)       (int, int);
typedef int  (*pVUserCB_t)       (int);

typedef struct {
    uint32_t eventPtr;
    uint32_t eventPopPtr;
    uint32_t eventQueue [MAX_QUEUED_VEC_IRQ];
} vecIrqState_t;

// openPCIE: the user code runs as a coroutine on the simulator's own thread,
// on a stack of its own, and VSched()/VExch() switch between the two stacks
// (VUser.c). Upstream ran it in a thread per node, handing over through two
// semaphores: two system calls and a scheduler wake-up on each exchange.
// x86-64 Linux; elsewhere, or with VPROC_THREADS defined, the thread version
#if !defined(VPROC_THREADS) && defined(__x86_64__) && defined(__linux__)
#define VPROC_COROUTINE
#endif

// Scheduler node state structure
typedef struct {
    sem_t               snd;
    sem_t               rcv;
    void               *co_sim_sp;      // openPCIE: the simulator's stack pointer, while the user code runs
    void               *co_user_sp;     // openPCIE: the user code's stack pointer, while the simulator runs
    send_buf_t          send_buf;
    rcv_buf_t           rcv_buf;
    pVUserIrqCB_t       VUserIrqCB;
    pPyIrqCB_t          PyIrqCB;
    vecIrqState_t       irqState;
    pVUserCB_t          VUserCB;
} SchedState_t, *pSchedState_t;

// Reference to node state array
extern pSchedState_t ns[];

#ifdef VPROC_COROUTINE
// openPCIE: switch stacks: save the callee-saved registers on the current
// stack, its pointer in *save, then continue on the stack 'load' (VUser.c)
extern void VCoSwap (void **save, void *load) __attribute__((visibility("hidden")));
#endif

#endif
