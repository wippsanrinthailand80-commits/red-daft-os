/* boot.S — Multiboot2 -> long mode -> identity-mapped 4GB -> kernel_main */
.code32
.section .multiboot, "a"
.align 8
mb2_header:
    .long 0xe85250d6
    .long 0
    .long mb2_end - mb2_header
    .long -(0xe85250d6 + 0 + (mb2_end - mb2_header))
    .long 0
    .long 8
mb2_end:

/* PVH ELF note: lets plain `qemu -kernel` load this 64-bit ELF.
 * We advertise _start as the 32-bit protected-mode physical entry
 * (paging off, flat segments, %ebx=start_info which we ignore).
 * Real firmware/GRUB keeps using the Multiboot2 header above. */
.section .note.Xen, "a"
.align 4
.long 4                  /* namesz ("Xen\0") */
.long 4                  /* descsz */
.long 18                 /* type: XEN_ELFNOTE_PHYS32_ENTRY */
.asciz "Xen"
.align 4
.long _start             /* desc: 32-bit physical entry point */

.section .rodata
.align 16
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF        /* 64-bit code */
    .quad 0x00AF92000000FFFF        /* 64-bit data */
gdt_end:
gdtr: .word gdt_end - gdt - 1; .long gdt

.section .bss
.align 4096
pml4:  .space 4096
pdpt:  .space 4096
pd0:   .space 4096
pd1:   .space 4096
pd2:   .space 4096
pd3:   .space 4096
stack_bottom:
.space 16384
stack_top:

.section .text
.global _start
_start:
    cli
    /* earliest possible serial beacon: init COM1 raw, print 'S' */
    mov $0x3F8, %edx
    mov $0x80, %al ; out %al, %dx        /* DLAB */
    mov $0x3F8, %edx
    mov $0x01, %al ; out %al, %dx        /* div lo =1 */
    inc %edx ; xor %al,%al ; out %al, %dx/* div hi */
    inc %edx ; mov $0x03, %al ; out %al,%dx /* 8N1 */
    inc %edx ; mov $0xC7, %al ; out %al,%dx /* fifo */
    inc %edx ; mov $0x0B, %al ; out %al,%dx /* modem */
    mov $'S', %al ; dec %edx ; dec %edx ; dec %edx ; dec %edx
    out %al, %dx                          /* TX -> 'S' */

    mark:                                  /* clobbers eax/edx */
    mov $stack_top, %esp
    and $-8, %esp

    /* zero .bss (freestanding: nobody does it for us) */
    lea bss_start, %edi
    lea bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx
    xor %eax, %eax
    cld
    rep stosl

    mov $'A', %al ; call beacon
    /* build identity page tables: PML4[0]->PDPT->4 PDs, 2MB pages = 4GB */
    lea pml4, %edi
    lea pdpt, %eax
    or  $3, %eax
    mov %eax, (%edi)

    lea pdpt, %edi
    lea pd0, %eax
    or  $3, %eax
    mov %eax, (%edi)                 /* PDPT[0] -> pd0 : 0..1GB */
    lea pd1, %eax
    or  $3, %eax
    mov %eax, 8(%edi)                /* PDPT[1] -> pd1 : 1..2GB */
    lea pd2, %eax
    or  $3, %eax
    mov %eax, 16(%edi)
    lea pd3, %eax
    or  $3, %eax
    mov %eax, 24(%edi)

    /* fill each PD with 512 x 2MB present|rw|huge entries */
    lea pd0, %edi
    mov $0x83, %eax                  /* first entry: phys 0 */
    mov $512*4, %ecx
fill:
    mov %eax, (%edi)
    add $0x200000, %eax              /* next 2MB */
    add $4, %edi
    loop fill

    mov $'B', %al ; call beacon
    /* enable PAE, page tables, long mode + paging */
    mov %cr4, %eax
    or  $0x20, %eax                  /* PAE */
    mov %eax, %cr4
    lea pml4, %eax
    mov %eax, %cr3
    mov $'C', %al ; call beacon
    mov $0xC0000080, %ecx            /* EFER MSR */
    rdmsr
    or  $0x100, %eax                 /* LME: long mode enable */
    wrmsr
    mov $'D', %al ; call beacon
    mov %cr0, %eax
    or  $0x80010000, %eax            /* PG | WP */
    mov %eax, %cr0

    lgdt gdtr
    ljmp $0x08, $long_mode

.code64
long_mode:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $stack_top, %rsp
    mov $'L', %al ; call beacon64
    /* pass multiboot info pointer in edi/ebx? keep ABI simple: zero args */
    xor %rdi, %rdi
    call kernel_main
halt:
    cli
    hlt
    jmp halt

/* ---- IRQ stubs: preserve caller-saved regs, call C handlers ---- */
beacon:                                   /* 32-bit: al = char */
    push %eax; push %edx
    mov $0x3F8, %edx
    out %al, %dx
    pop %edx; pop %eax
    ret
beacon64:                                 /* 64-bit */
    push %rax; push %rdx
    mov $0x3F8, %edx
    out %al, %dx
    pop %rdx; pop %rax
    ret

.global irq0_stub
irq0_stub:
    push %rax; push %rcx; push %rdx; push %rsi; push %rdi
    push %r8;  push %r9;  push %r10; push %r11
    cld
    call irq_timer_c
    pop %r11; pop %r10; pop %r9; pop %r8
    pop %rdi; pop %rsi; pop %rdx; pop %rcx; pop %rax
    iretq

.global irq1_stub
irq1_stub:
    push %rax; push %rcx; push %rdx; push %rsi; push %rdi
    push %r8;  push %r9;  push %r10; push %r11
    cld
    call irq_kbd_c
    pop %r11; pop %r10; pop %r9; pop %r8
    pop %rdi; pop %rsi; pop %rdx; pop %rcx; pop %rax
    iretq

/* mark stack as non-executable (silences GNU-stack warning) */
.section .note.GNU-stack,"",@progbits
