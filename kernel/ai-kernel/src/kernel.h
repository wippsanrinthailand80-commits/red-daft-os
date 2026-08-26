/* kernel.h — Red Daft AI-Kernel v0.2 "Demo Box"
 * From-scratch x86_64 kernel: buddy PMM, kernel heap, IRQs, spinlocks,
 * HMM v2 (VRAM-as-cache with restore/donate, LRU+freq eviction,
 * prefetch, dirty writeback) and an interactive serial demo box. */
#ifndef AIK_KERNEL_H
#define AIK_KERNEL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef signed   char  s8;
typedef signed   short s16;
typedef signed   int   s32;
typedef signed   long  s64;
typedef unsigned long  usize;
#define NULL ((void*)0)

static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb(u16 p){ u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void io_wait(void){ outb(0x80,0); }

void kprintf(const char*, ...);
void panic(const char *msg) __attribute__((noreturn));

/* serial */
void serial_init(void);
void serial_putc(char c);
char serial_getc(void);          /* blocks */
int  serial_pollc(void);         /* -1 if empty */

/* console (serial mirror) */
void con_init(void);
void con_putc(char c);
void con_puts(const char*);
void dbgmark(char);

/* string */
usize kstrlen(const char*);
int  kstrcmp(const char*, const char*);
void *kmemset(void*, int, usize);
void *kmemcpy(void*, const void*, usize);

/* pmm: buddy page allocator (4KB pages) */
void  pmm_init(u64 *mmap_entries, u32 n);
u64   pmm_alloc(void);            /* phys addr of one zeroed-candidate page */
void  pmm_free(u64 pa);
u64   pmm_total(void), pmm_free_pages(void);
/* donate/restore: HMM grows/shrinks its pools through here */
u64   pmm_donate_range(u32 count); /* carve contiguous run (order-rounded) */
void  pmm_restore_block(u64 pa, int order, u32 count);

/* heap */
void  heap_init(void);
void *kmalloc(usize);
void  kfree(void*);

/* locks */
typedef volatile u32 spinlock_t;
#define SPIN_INIT 0
void spin_lock(spinlock_t*);
void spin_unlock(spinlock_t*);

/* interrupts/time */
void idt_init(void);
void pit_init(u32 hz);
u64  ticks(void);
void sleep_ticks(u64);
char kbd_pollkey(void);           /* non-blocking, 0 when empty */
void irq_timer_c(void);
void irq_kbd_c(void);

/* pci */
void pci_scan(void);

/* reboot — 8042 + CF9 + triple-fault fallback */
void reboot(void) __attribute__((noreturn));
void reboot_to(const char *target) __attribute__((noreturn));

/* models */
void models_init(void);
int  model_verify(u32 m);
int  model_verify_trained(u32 m);
s64  train_model(u32 m);
const char *model_name(u32 m);
u32  model_count(void);
void shell_run(void) __attribute__((noreturn));

#endif
