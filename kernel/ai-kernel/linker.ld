ENTRY(_start)

SECTIONS {
    . = 0x100000;                /* load at 1MB (Multiboot convention) */

    .text : {
        *(.multiboot)           /* Multiboot2 header MUST be first */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        _end = .;
    }

    /DISCARD/ : { *(.note*) *(.eh_frame*) *(.comment) }
}
