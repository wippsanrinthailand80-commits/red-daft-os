#include "kernel.h"

/* --- Physical page allocator (free-list of 4KB pages) ---
 * Pages are identity-mapped (see boot.S), so a physical address is also a
 * usable virtual pointer. The free list stores the next pointer inside the
 * page itself (embedded list), so no separate metadata is needed. */

static u64 free_head = 0;
static u64 page_count = 0;

struct mb2_fixed { u32 total_size; u32 reserved; };
struct mb2_tag   { u32 type; u32 size; };
struct mb2_mmap { u32 type; u32 size; u32 entry_size; u32 entry_version; };
struct mb2_mmap_entry { u64 base; u64 length; u32 e_type; u32 reserved; };

void pmm_free_page(u64 p){
    *(u64 *)p = free_head;   /* embed next pointer in the page */
    free_head = p;
    page_count++;
}

u64 pmm_alloc(void){
    if (!free_head){ kprintf("PMM: OUT OF MEMORY\n"); for(;;); }
    u64 p = free_head;
    free_head = *(u64 *)p;
    page_count--;
    return p;
}

u64 pmm_pages(void){ return page_count; }

/* Add a usable region [base, base+len) as 4KB pages, skipping the first 4MB
 * (reserved for kernel/stack/page tables) and aligning up to page boundary. */
static void add_region(u64 base, u64 len){
    if (base < 0x400000ULL){
        if (base + len <= 0x400000ULL) return;
        len  -= (0x400000ULL - base);
        base  = 0x400000ULL;
    }
    u64 aligned = (base + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);
    len -= (aligned - base);
    base = aligned;
    u64 p = base, pend = base + len;
    while (p + PAGE_SIZE <= pend){ pmm_free_page(p); p += PAGE_SIZE; }
}

void pmm_init(void *mbi){
    struct mb2_fixed *f = (void *)mbi;
    u32 off = 8;
    while (off < f->total_size){
        struct mb2_tag *t = (void *)((u8 *)f + off);
        if (t->type == 0) break;
        if (t->type == 6){                       /* memory map */
            struct mb2_mmap *m = (void *)t;
            u8 *e   = (u8 *)m + sizeof(struct mb2_mmap);
            u8 *end = (u8 *)t + t->size;
            while (e + sizeof(struct mb2_mmap_entry) <= end){
                struct mb2_mmap_entry *en = (void *)e;
                if (en->e_type == 1) add_region(en->base, en->length);
                e += m->entry_size;
            }
        }
        off += (t->size + 7) & ~7u;
    }
    if (page_count == 0){
        kprintf("PMM: mmap unavailable; using fallback 16MB arena @0x800000\n");
        add_region(0x800000ULL, 16ULL * 1024 * 1024);
    }
}
