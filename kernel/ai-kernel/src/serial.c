#include "kernel.h"

void serial_init(void){
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor low  = 1  -> 115200 baud */
    outb(COM1 + 1, 0x00);   /* divisor high = 0 */
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* enable FIFO */
    outb(COM1 + 1, 0x00);   /* disable IRQ */
}

void serial_putc(char c){
    while (!(inb(COM1 + 5) & 0x20));   /* wait for THR empty */
    outb(COM1, (u8)c);
}
