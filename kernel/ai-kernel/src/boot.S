/* boot.S - from-scratch x86_64 entry for Multiboot2 (QEMU -kernel).
 * Responsibilities: zero BSS, build identity page tables (0..512MB, 2MB pages),
 * enable PAE+long mode, load a 64-bit GDT, then call kernel_main(mbi) in 64-bit.
 * NO vendor/firmware dependencies: this is pure CPU bootstrap. */

.code32
.section .multiboot, "a"
.align 8
mb2_header:
    .long 0xe85250d6            /* Multiboot2 magic */
    .long 0                     /* architecture = 0 (x86 32-bit) */
    .long mb2_header_end - mb2_header
    .long -(0xe85250d6 + 0 + (mb2_header_end - mb2_header))
    .long 0                     /* terminating tag: type 0 */
    .long 8                     /* terminating tag: size 8 */
mb2_header_end:

.section .rodata
/* Minimal GDT: null, 64-bit code (0x08), 64-bit data (0x10). */
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF    /* 64-bit code */
    .quad 0x00AF92000000FFFF    /* 64-bit data */
gdt_end:
gdtr:
    .word gdt_end - gdt - 1
    .long gdt

.section .bss, "aw"
.align 4096
.global pml4
pml4:  .skip 4096
.global pdpt
pdpt:  .skip 4096
.global pd0
pd0:   .skip 4096
.align 16
boot_stack:     .skip 65536
.global boot_stack_top
boot_stack_top:

.section .text
.global _start
_start:
    cld
    /* Zero BSS (page tables live here; must be cleared before use). */
    movl $__bss_start, %edi
    movl $_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    movl $boot_stack_top, %esp
    movl %ebx, %esi            /* stash Multiboot2 info pointer */

    /* PML4[0] -> PDPT */
    movl $pml4, %eax
    movl $pdpt, %ebx
    orl  $0x3, %ebx
    movl %ebx, (%eax)

    /* PDPT[0] -> PD0 */
    movl $pdpt, %eax
    movl $pd0, %ebx
    orl  $0x3, %ebx
    movl %ebx, (%eax)

    /* PD0[i] -> 2MB identity-mapped page i */
    movl $0, %ecx
    movl $pd0, %edx
1:
    movl %ecx, %eax
    shll $21, %eax             /* i * 2MB */
    orl  $0x83, %eax           /* P | RW | PS(2MB) */
    movl %eax, (%edx, %ecx, 8)
    incl %ecx
    cmpl $256, %ecx
    jl   1b

    /* Enable PAE */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4
    /* Load CR3 */
    movl $pml4, %eax
    movl %eax, %cr3
    /* Set EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr
    /* Enable paging -> IA-32e compatibility mode */
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    lgdt gdtr
    ljmp $0x08, $start64        /* switch to 64-bit code segment */

.code64
start64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $boot_stack_top, %rsp
    movl %esi, %edi            /* mbi -> rdi (zero-extended) */
    call kernel_main
    hlt
