/* serial.c — COM1 115200 8N1 */
#include "kernel.h"

#define COM1 0x3F8

void serial_init(void){
    outb(COM1+1,0x00);      /* no irqs */
    outb(COM1+3,0x80);      /* DLAB */
    outb(COM1+0,0x01);      /* div 1 => 115200 */
    outb(COM1+1,0x00);
    outb(COM1+3,0x03);      /* 8N1 */
    outb(COM1+2,0xC7);      /* fifo */
    outb(COM1+4,0x0B);      /* DTR|RTS|OUT2 */
}

void serial_putc(char c){
    for(int i=0;i<100000 && !(inb(COM1+5)&0x20);i++) io_wait();
    outb(COM1,c);
}

char serial_getc(void){
    while(!(inb(COM1+5)&0x01)) io_wait();
    return (char)inb(COM1);
}

int serial_pollc(void){
    if(!(inb(COM1+5)&0x01)) return -1;
    return inb(COM1);
}
