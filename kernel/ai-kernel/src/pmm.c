/* pmm.c — buddy page allocator fed by the Multiboot2 memory map.
 * Orders 0..10 (4KB..4MB). Free lists of physical addresses.
 * Also provides the donate/restore seam the HMM uses for its elastic pool. */
#include "kernel.h"

#define MAX_ORDER 10
#define PAGE_SIZE 4096u

static spinlock_t pmm_lock = SPIN_INIT;
static u64 free_list[MAX_ORDER+1];      /* head phys addr or 0 */
static u64 total_pages, free_pg;
static u64 arena_base, arena_end;       /* fallback region */

/* each free page stores its next-pointer + order in its own memory */
typedef struct { u64 next; u8 order; } free_hdr_t;

static inline void fl_push(int o, u64 pa){
    free_hdr_t *h = (free_hdr_t*)(u32)pa;   /* identity-mapped low RAM */
    h->order = (u8)o; h->next = free_list[o]; free_list[o] = pa;
}
static inline u64 fl_pop(int o){
    u64 pa = free_list[o];
    if(pa){ free_hdr_t *h=(free_hdr_t*)(u32)pa; free_list[o]=h->next; }
    return pa;
}

void pmm_init(u64 *entries, u32 n){
    (void)entries;(void)n;
    /* Fallback arena: 64MB @ 32MB. Kept ABOVE kernel_end (the demo-box
     * .bss holds ~20MB of model backing store below that). The boot block
     * can pass the real Multiboot2 map later; without it we still run. */
    arena_base = 0x2000000; arena_end = 0x6000000;
    total_pages = (arena_end-arena_base)/PAGE_SIZE;
    kmemset(free_list, 0, sizeof(free_list));
    u64 cur = arena_base;
    while(cur < arena_end){ fl_push(MAX_ORDER, cur); cur += PAGE_SIZE<<MAX_ORDER; }
    free_pg = total_pages;
}

u64 pmm_alloc(void){
    spin_lock(&pmm_lock);
    int o = 0;
    while(o<=MAX_ORDER && !free_list[o]) o++;
    if(o>MAX_ORDER){ spin_unlock(&pmm_lock); return 0; }
    u64 pa = fl_pop(o);
    while(o>0){                       /* split down */
        o--;
        fl_push(o, pa + (PAGE_SIZE<<o));
    }
    free_pg--;
    spin_unlock(&pmm_lock);
    return pa;
}

void pmm_free(u64 pa){
    spin_lock(&pmm_lock);
    /* coalesce up as far as buddies are aligned+free (linear scan of list) */
    int o=0;
    while(o<MAX_ORDER){
        u64 buddy = pa ^ (PAGE_SIZE<<o);
        u64 *p = &free_list[o], found=0;
        while(*p){
            if(*p==buddy){ found=1; break; }
            p = &((free_hdr_t*)(u32)*p)->next;
        }
        if(!found) break;
        *p = ((free_hdr_t*)(u32)buddy)->next;   /* unlink buddy */
        if(buddy<pa) pa=buddy;
        o++;
    }
    fl_push(o,pa);
    free_pg++;
    spin_unlock(&pmm_lock);
}

/* Contiguous run carve-out for elastic pools (best effort).
 * Returns base; caller must return the SAME order via pmm_restore_block
 * so buddy geometry stays consistent. */
u64 pmm_donate_range(u32 count){
    if(!count) return 0;
    int need_order = 0; while((1u<<need_order) < count && need_order<MAX_ORDER) need_order++;
    spin_lock(&pmm_lock);
    int o=need_order;
    while(o<=MAX_ORDER && !free_list[o]) o++;
    if(o>MAX_ORDER){ spin_unlock(&pmm_lock); return 0; }
    u64 pa = fl_pop(o);
    while(o>need_order){ o--; fl_push(o, pa+(PAGE_SIZE<<o)); }
    free_pg -= count;
    spin_unlock(&pmm_lock);
    return pa;
}

void pmm_restore_block(u64 pa, int order, u32 count){
    if(order<0 || order>MAX_ORDER || !pa) return;
    spin_lock(&pmm_lock);
    fl_push(order, pa);
    free_pg += count;
    spin_unlock(&pmm_lock);
}

u64 pmm_total(void){ return total_pages*PAGE_SIZE; }
u64 pmm_free_pages(void){ return free_pg; }
