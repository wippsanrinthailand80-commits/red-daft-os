#ifndef KERNEL_H
#define KERNEL_H

/* Fixed-width types (freestanding: no <stdint.h> available). */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed int          s32;
typedef signed long long    s64;

/* Minimal stdarg via compiler builtins (works with -nostdinc). */
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_arg(v,t)   __builtin_va_arg(v,t)
#define va_end(v)     __builtin_va_end(v)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* --- Serial (COM1) for deterministic QEMU output --- */
#define COM1 0x3F8
static inline void outb(u16 port, u8 v){ asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline u8  inb(u16 port){ u8 r; asm volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }

void serial_init(void);
void serial_putc(char c);
void kprintf(const char *fmt, ...);

/* --- Physical memory manager --- */
void pmm_init(void *mbi);
u64  pmm_alloc(void);
u64  pmm_pages(void);

/* --- AI sandbox entry point --- */
void sandbox_run(void);

#endif
