/* pci.c — enumerate bus 0..255 config space, report mass-storage/net/display */
#include "kernel.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static u32 pci_read(u32 bus,u32 dev,u32 fn,u32 off){
    u32 a = 0x80000000u | (bus<<16) | (dev<<11) | (fn<<8) | (off&0xFC);
    __asm__ volatile("outl %0,%1"::"a"(a),"Nd"(CFG_ADDR));
    u32 r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(CFG_DATA));
    return r;
}

static const char *cls_name(u8 c){
    switch(c){
    case 0: return "legacy";
    case 1: return "storage";
    case 2: return "network";
    case 3: return "display";
    case 6: return "bridge";
    case 0x0C: return "serial-bus";
    default: return "other";
    }
}

void pci_scan(void){
    int found=0;
    for(u32 bus=0; bus<8; bus++)
      for(u32 dev=0; dev<32; dev++)
        for(u32 fn=0; fn<8; fn++){
            u32 v = pci_read(bus,dev,fn,0);
            if(v==0xFFFFFFFF) continue;
            u32 cls = pci_read(bus,dev,fn,8);
            u8 base = (cls>>24)&0xFF;
            kprintf("[pci] %u:%u.%u %s ven=%x dev=%x\n",
                    bus,dev,fn,cls_name(base), v&0xFFFF, v>>16);
            found++;
        }
    if(!found) kprintf("[pci] none enumerated\n");
}
