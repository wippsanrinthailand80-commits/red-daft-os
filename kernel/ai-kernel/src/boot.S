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
    mov $0xE9, %edx
    mov $'S', %al
    out %al, %dx                          /* debugcon -> 'S' */

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

    /* identity map 0..2GB with two 1GB huge pages:
     *   pml4[0] -> pdpt ; pdpt[0] = phys 0GB ; pdpt[1] = phys 1GB */
    lea pml4, %edi
    lea pdpt, %eax
    or  $3, %eax
    mov %eax, (%edi)                 /* PML4[0] -> PDPT, present|rw */

    lea pdpt, %edi
    mov $0x83, %eax                  /* PDPT[0]: 1GB page @ phys 0 */
    mov %eax, (%edi)
    mov $0x40000083, %eax            /* PDPT[1]: 1GB page @ phys 1GB */
    mov %eax, 8(%edi)

    mov $'B', %al ; call beacon
    /* enable PAE, load tables + OUR GDT, then switch on paging */
    mov %cr4, %eax
    or  $0x20, %eax                  /* PAE */
    mov %eax, %cr4
    lea pml4, %eax
    mov %eax, %cr3
    mov $'C', %al ; call beacon

    lgdt gdtr                        /* our GDT before paging, always */
    mov $'D', %al ; call beacon

    mov $0xC0000080, %ecx            /* EFER MSR */
    rdmsr
    or  $0x100, %eax                 /* LME: long mode enable */
    wrmsr
    mov $'E', %al ; call beacon

    mov %cr0, %eax
    or  $0x80010000, %eax            /* PG | WP */
    mov %eax, %cr0
    jmp 1f                           /* flush pipeline with paging on */
1:  mov $'F', %al ; call beacon

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

/* ---- serial beacons + IRQ stubs ---- */
.code32
beacon:                                   /* 32-bit: al = char (serial+E9) */
    push %eax; push %edx
    mov $0x3F8, %edx
    out %al, %dx
    mov $0xE9, %edx                       /* qemu debugcon mirror */
    out %al, %dx
    pop %edx; pop %eax
    ret

.code64
beacon64:                                 /* 64-bit */
    push %rax; push %rdx
    mov $0x3F8, %edx
    out %al, %dx
    mov $0xE9, %edx
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
