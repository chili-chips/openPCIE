// Reset entry: zero .bss (a link drop resets the SOC, not its RAM, so the
// globals would keep their last values), set the stack to the top of RAM,
// run main
.section .vectors, "ax"
.global _start
_start:
    la   t0, __bss_start
    la   t1, __bss_end
1:  bgeu t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b
2:  la   sp, _stack_top
    call main
loop:
    j loop

// -----------------------------------------------------------------------------
// Project:     openPCIE
// Description: NLnet-sponsored open-source implementation
// Version:     1.0
// Date:        May 24, 2024
// -----------------------------------------------------------------------------
