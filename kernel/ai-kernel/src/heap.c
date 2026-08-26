/* heap.c — first-fit free-list allocator over pages from the PMM */
#include "kernel.h"

typedef struct blk { usize size; struct blk *next; int free; } blk_t;

static spinlock_t lock = SPIN_INIT;
static blk_t *head;
#define HEAP_PAGES 256   /* 1MB heap */

void heap_init(void){
    u64 pa = pmm_donate_range(HEAP_PAGES);
    if(!pa) panic("heap: no arena");
    head = (blk_t*)(usize)pa;
    head->size = HEAP_PAGES*4096 - sizeof(blk_t);
    head->next = NULL; head->free = 1;
}

void *kmalloc(usize n){
    n = (n + 15) & ~15ul;
    spin_lock(&lock);
    for(blk_t *b=head; b; b=b->next){
        if(b->free && b->size >= n){
            if(b->size >= n + sizeof(blk_t) + 32){
                blk_t *nb = (blk_t*)((char*)b + sizeof(blk_t) + n);
                nb->size = b->size - n - sizeof(blk_t);
                nb->free = 1; nb->next = b->next;
                b->next = nb; b->size = n;
            }
            b->free = 0;
            spin_unlock(&lock);
            return (char*)b + sizeof(blk_t);
        }
    }
    spin_unlock(&lock);
    return NULL;
}

void kfree(void *p){
    if(!p) return;
    spin_lock(&lock);
    blk_t *b = (blk_t*)((char*)p - sizeof(blk_t));
    b->free = 1;
    for(blk_t *c=head; c && c->next; c=c->next)
        if(c->free && c->next->free){
            c->size += sizeof(blk_t) + c->next->size;
            c->next = c->next->next;
        }
    spin_unlock(&lock);
}
