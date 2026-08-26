/* printf.c — tiny kprintf using GCC builtins (freestanding, no stdarg.h)
 * supports: %d %u %x %p %s %c %lld %llu %% */
#include "kernel.h"

static void putn(const char *s, usize n){ while(n--) con_putc(*s++); }
static void puts_(const char *s){ putn(s, kstrlen(s)); }

static void num(u64 v, int base, int sgn){
    char b[32]; const char *d="0123456789abcdef";
    int i=31; int neg = sgn && (s64)v<0;
    u64 x = neg ? (u64)(-(s64)v) : v;
    if(!x) b[i--]='0';
    while(x){ b[i--]=d[x%base]; x/=base; }
    if(neg) b[i--]='-';
    putn(&b[i+1], 31-i);
}

void kprintf(const char *f, ...){
    __builtin_va_list ap; __builtin_va_start(ap,f);
    for(; *f; f++){
        if(*f!='%'){ con_putc(*f); continue; }
        switch(*++f){
        case 'd': num((u64)(long long)__builtin_va_arg(ap,int),10,1); break;
        case 'u': num((u64)__builtin_va_arg(ap,unsigned),10,0); break;
        case 'l':
            if(f[1]=='l'&&f[2]=='u'){ num(__builtin_va_arg(ap,u64),10,0); f+=2; }
            else if(f[1]=='l'&&f[2]=='d'){ num((u64)__builtin_va_arg(ap,s64),10,1); f+=2; }
            break;
        case 'x': num((u64)__builtin_va_arg(ap,unsigned),16,0); break;
        case 'p': puts_("0x"); num((u64)__builtin_va_arg(ap,void*),16,0); break;
        case 's':{ const char*s=__builtin_va_arg(ap,const char*); puts_(s?s:"(null)"); break; }
        case 'c': con_putc((char)__builtin_va_arg(ap,int)); break;
        case '%': con_putc('%'); break;
        default : con_putc('%'); con_putc(*f);
        }
    }
    __builtin_va_end(ap);
}

void panic(const char *msg){
    con_puts("\nPANIC: "); con_puts(msg);
    con_puts("\nSystem halted.\n");
    for(;;){ __asm__ volatile("cli; hlt"); }
}
