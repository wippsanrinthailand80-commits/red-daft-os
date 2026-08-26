/* string.c */
#include "kernel.h"

usize kstrlen(const char *s){ usize n=0; while(s[n]) n++; return n; }
int kstrcmp(const char *a, const char *b){
    while(*a && *a==*b){ a++; b++; }
    return (u8)*a - (u8)*b;
}
void *kmemset(void *d, int c, usize n){
    u8 *p=d; while(n--) *p++=(u8)c; return d;
}
void *kmemcpy(void *d, const void *s, usize n){
    u8 *a=d; const u8 *b=s; while(n--) *a++=*b++; return d;
}
