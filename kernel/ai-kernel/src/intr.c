/* intr.c — IDT, PIC remap, PIT timer, PS/2 keyboard */
#include "kernel.h"

static volatile u64 tick_count;
static spinlock_t kbd_lock = SPIN_INIT;
static char kbd_q[32]; static volatile u32 kbd_r, kbd_w;

struct idt_entry { u16 lo, sel; u8 zero, flags; u16 hi; } __attribute__((packed));
struct idt_ptr  { u16 limit; u64 base; } __attribute__((packed));
static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void irq0_stub(void); extern void irq1_stub(void);
/* thunks in boot.S: push vector, jmp common */
extern void intr_common(u64 vec);

static void set_gate(int n, u64 handler){
    idt[n].lo   = handler & 0xFFFF;
    idt[n].sel  = 0x08;
    idt[n].zero = 0;
    idt[n].flags= 0x8E;               /* present, ring0, interrupt gate */
    idt[n].hi   = handler >> 16;
}

void irq_timer_c(void){ tick_count++; outb(0x20,0x20); }
void irq_kbd_c(void){
    u8 sc = inb(0x60);
    if(sc & 0x80) goto done;          /* key release */
    static const char map[] = "\0?1234567890-=\b?qwertyuiop[]\n?asdfghjkl;'`?\\zxcvbnm,./";
    char c = (sc < sizeof(map)) ? map[sc] : 0;
    if(c){
        spin_lock(&kbd_lock);
        u32 nw=(kbd_w+1)%sizeof(kbd_q);
        if(nw!=kbd_r){ kbd_q[kbd_w]=c; kbd_w=nw; }
        spin_unlock(&kbd_lock);
    }
done:
    outb(0x20,0x20);
}

u64 ticks(void){ return tick_count; }
void sleep_ticks(u64 t){ u64 end=tick_count+t; while(tick_count<end) __asm__ volatile("hlt"); }

/* non-blocking: 0 when queue empty */
char kbd_pollkey(void){
    spin_lock(&kbd_lock);
    if(kbd_r==kbd_w){ spin_unlock(&kbd_lock); return 0; }
    char c=kbd_q[kbd_r]; kbd_r=(kbd_r+1)%sizeof(kbd_q);
    spin_unlock(&kbd_lock);
    return c;
}

void idt_init(void){
    kmemset(idt,0,sizeof(idt));
    set_gate(32,(u64)irq0_stub);
    set_gate(33,(u64)irq1_stub);
    idtp.limit = sizeof(idt)-1; idtp.base = (u64)idt;
    __asm__ volatile("lidt %0"::"m"(idtp));
    /* remap PIC to 32..47 */
    outb(0x20,0x11); io_wait(); outb(0xA0,0x11); io_wait();
    outb(0x21,0x20); outb(0xA1,0x28);
    outb(0x21,0x04); outb(0xA1,0x02);
    outb(0x21,0x01); outb(0xA1,0x01);
    outb(0x21,0x00); outb(0xA1,0x00);
    __asm__ volatile("sti");
}

void pit_init(u32 hz){
    u32 div = 1193182/hz;
    outb(0x43,0x36);
    outb(0x40,div&0xFF); outb(0x40,(div>>8)&0xFF);
}
