/* hmm.h — Heterogeneous Memory Manager v2.
 * A small device-side pool ("VRAM") is kept as a paged cache over a much
 * larger host-RAM model. The pool size is elastic: pages are DONATED from
 * the physical allocator when the working set grows and RESTORED to it
 * under pressure ("restores RAM from the main kernel").
 *
 * Policy: LRU primary, access-frequency tie-break (2-bit counter),
 * sequential prefetch window on miss, dirty-page writeback. All state is
 * spinlock-protected; IRQs are not expected to touch the HMM. */
#ifndef AIK_HMM_H
#define AIK_HMM_H

#include "kernel.h"

#define HMM_PAGE_SIZE   4096u

typedef struct hmm_page {
    struct hmm_page *lru_prev, *lru_next;  /* pool LRU list */
    u64 host_addr;                          /* backing store in RAM */
    u64 vram_addr;                          /* current device location */
    u32 model_id, page_idx;
    u8  freq        : 2;                    /* 0..3, decayed on evict */
    u8  dirty       : 1;
    u8  resident    : 1;
} hmm_page_t;

typedef struct {
    const char *name;
    u64     host_base;                      /* model bytes live here */
    u64     npages;                         /* model size in pages */
} hmm_model_t;

typedef struct {
    spinlock_t lock;
    /* elastic pool bounds (physical addresses donated by PMM) */
    u64  pool_base, pool_pages, pool_cap;
    /* LRU list */
    hmm_page_t *lru_head, *lru_tail;
    /* page table for resident pages: hash by (model_id,page_idx) */
    #define HMM_HASH_BITS 10
    hmm_page_t *htab[1u<<HMM_HASH_BITS];
    /* stats */
    u64 faults, hits, misses, prefetched, evictions, writebacks;
    u64 bytes_migrated, donated_now, restored_ever;
} hmm_t;

int  hmm_init(u64 initial_pool_pages, u64 max_pool_pages);
int  hmm_register_model(const char *name, void *host_buf, u64 npages);
int  hmm_model_id(const char *name);
/* fault-in a model page: returns device address or 0 on failure */
u64  hmm_fault(u32 model, u32 idx);
/* explicit write path: mark dirty + migrate in */
u64  hmm_write_fault(u32 model, u32 idx);
/* shrink pool by n pages (evict LRU tail), returns pages actually freed */
u64  hmm_restore_to_pmm(u32 n);
void hmm_stats(void);
u64  hmm_amplification_x100(void);   /* model_bytes*100 / pool_bytes */
u64  hmm_pool_pages(void);

#endif
