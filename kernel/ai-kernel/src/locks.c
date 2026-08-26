/* locks.c */
#include "kernel.h"

void spin_lock(spinlock_t *l){
    for(;;){
        while(*l) __asm__ volatile("pause");
        u32 ok=1;
        __asm__ volatile("lock xchg %0,%1" : "+r"(ok), "+m"(*l)::"memory");
        if(!ok) break;              /* old value 0 => we own it */
    }
}
void spin_unlock(spinlock_t *l){
    __asm__ volatile("movl $0,%0" : "+m"(*l)::"memory");
}
