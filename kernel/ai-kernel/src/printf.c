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
        /* skip flags/width/precision so every directive consumes exactly
         * one argument — prevents silent varargs desync. */
        const char *spec=f+1;
        while(*spec=='-'||*spec=='0'||*spec=='+'||*spec==' '||
              (*spec>='0'&&*spec<='9')||*spec=='.') spec++;
        char c=*spec;
        switch(c){
        case 'd': num((u64)(long long)__builtin_va_arg(ap,int),10,1); f=(char*)spec; break;
        case 'u': num((u64)__builtin_va_arg(ap,unsigned),10,0);       f=(char*)spec; break;
        case 'x': num((u64)__builtin_va_arg(ap,unsigned),16,0);       f=(char*)spec; break;
        case 'p': puts_("0x"); num((u64)__builtin_va_arg(ap,void*),16,0); f=(char*)spec; break;
        case 's':{const char*s=__builtin_va_arg(ap,const char*);puts_(s?s:"(null)");f=(char*)spec;break;}
        case 'c': con_putc((char)__builtin_va_arg(ap,int));           f=(char*)spec; break;
        case 'l':
            if(spec[0]=='l'&&spec[1]=='l'&&(spec[2]=='d'||spec[2]=='u')){
                num(__builtin_va_arg(ap,u64),10,spec[2]=='d');
                f=(char*)spec+2;
            } else {
                con_putc('%');
            }
            break;
        case '%': con_putc('%'); f=(char*)spec; break;
        default : con_putc('%'); con_putc(c);
        }
    }
    __builtin_va_end(ap);
}

void panic(const char *msg){
    con_puts("\nPANIC: "); con_puts(msg);
    con_puts("\nSystem halted.\n");
    for(;;){ __asm__ volatile("cli; hlt"); }
}
