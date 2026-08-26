/* linker.ld — .multiboot first at 1MB, then text/rodata/data/bss */
ENTRY(_start)
SECTIONS
{
    . = 1M;
    .multiboot : { KEEP(*(.multiboot)) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { bss_start = .; *(COMMON) *(.bss*) ; bss_end = .; }
    kernel_end = .;
}
