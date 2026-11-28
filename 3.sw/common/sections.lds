/* Code, constants, data and stack share the SOC RAM. The stack grows down
   from its top; at least STACK_MIN bytes stay free for it. */
MEMORY {
    mem : ORIGIN = 0x00000000, LENGTH = 0x00002000
}
STACK_MIN = 0x400;
SECTIONS {
    .text : {
        *(.vectors)
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
    } > mem
    .data : {
        *(.data .data.* .sdata .sdata.*)
    } > mem
    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > mem
    _stack_top = ORIGIN(mem) + LENGTH(mem);
    /* A link drop resets the SOC, not its RAM: .bss is zeroed again at
       start-up, .data is not reloaded. So no initialised mutable globals */
    ASSERT(SIZEOF(.data) == 0, "initialised globals: .data is not reloaded after a reset")
    ASSERT(__bss_end + STACK_MIN <= _stack_top, "firmware leaves less than STACK_MIN bytes of stack")
}

/* -----------------------------------------------------------------------------
   Project:     openPCIE
   Description: NLnet-sponsored open-source implementation
   Version:     1.0
   Date:        May 24, 2024
   ----------------------------------------------------------------------------- */
