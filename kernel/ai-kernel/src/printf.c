#include "kernel.h"

static void putu(u64 v, u32 base, int upper){
    if (v == 0){ serial_putc('0'); return; }
    char buf[24]; int i = 0;
    while (v){
        u32 d = (u32)(v % base);
        buf[i++] = (d < 10) ? ('0' + d) : ((upper ? 'A' : 'a') + (d - 10));
        v /= base;
    }
    while (i) serial_putc(buf[--i]);
}

static void puts_(const char *s){ while (*s) serial_putc(*s++); }

void kprintf(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    while (*fmt){
        if (*fmt != '%'){ serial_putc(*fmt++); continue; }
        fmt++;
        if (*fmt == 'l'){                 /* long modifier */
            fmt++;
            if (*fmt == 'l'){             /* %lld / %llu */
                fmt++;
                if (*fmt == 'd'){
                    s64 v = va_arg(ap, s64);
                    if (v < 0){ serial_putc('-'); putu((u64)(-v), 10, 0); }
                    else putu((u64)v, 10, 0);
                } else if (*fmt == 'u'){
                    putu(va_arg(ap, u64), 10, 0);
                }
            }
        } else if (*fmt == 'd' || *fmt == 'i'){
            s32 v = va_arg(ap, s32);
            if (v < 0){ serial_putc('-'); putu((u64)(-v), 10, 0); }
            else putu((u64)v, 10, 0);
        } else if (*fmt == 'u'){
            putu(va_arg(ap, u32), 10, 0);
        } else if (*fmt == 'x'){
            putu(va_arg(ap, u32), 16, 0);
        } else if (*fmt == 'X'){
            putu(va_arg(ap, u32), 16, 1);
        } else if (*fmt == 's'){
            puts_(va_arg(ap, char *));
        } else if (*fmt == 'c'){
            serial_putc((char)va_arg(ap, int));
        } else if (*fmt == '%'){
            serial_putc('%');
        }
        fmt++;
    }
    va_end(ap);
}
