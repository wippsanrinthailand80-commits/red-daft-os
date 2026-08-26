/* reboot.c — bare-metal reboot via 8042 / CF9 / triple-fault */
#include "kernel.h"

static void delay(void){ for(volatile int i=0;i<100000;i++) io_wait(); }

/* low-level pulse: try the classic keyboard-controller reset */
static void kbd_reset(void){
    /* wait for input buffer empty */
    for(int i=0;i<100;i++){
        if((inb(0x64)&2)==0) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    delay();
}

/* PCI reset via port CF9 (available on QEMU + most x86) */
static void cf9_reset(void){
    outb(0xCF9, 0x02); delay();
    outb(0xCF9, 0x0E); delay();
}

void reboot(void){
    kprintf("[reboot] 8042 pulse...\n");
    kbd_reset();
    kprintf("[reboot] CF9 pulse...\n");
    cf9_reset();
    kprintf("[reboot] triple-fault...\n");
    struct { u16 lim; u64 base; } __attribute__((packed)) idt0 = {0,0};
    __asm__ volatile("lidt %0" :: "m"(idt0));
    __asm__ volatile("int $0");
    for(;;) __asm__ volatile("hlt");
}

void reboot_to(const char *target){
    if(target && *target)
        kprintf("[reboot] next target: %s\n", target);
    kprintf("[reboot] on installed systems: 'daft-kernel set %s' on the Linux side makes it persistent via GRUB.\n",
            target && *target ? target : "next");
    reboot();
}
