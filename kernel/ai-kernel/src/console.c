/* console.c — serial console with minimal line discipline */
#include "kernel.h"

void con_putc(char c){
    if(c=='\n') serial_putc('\r');
    serial_putc(c);
}
void con_puts(const char *s){ while(*s) con_putc(*s++); }
void con_init(void){ /* serial already up */ }

/* one-char breadcrumb to the qemu debugcon (0xE9) — boot debugging aid */
void dbgmark(char c){ outb(0xE9, (u8)c); }

