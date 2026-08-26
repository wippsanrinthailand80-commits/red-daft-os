/* hmm.h — Heterogeneous Memory Manager v3: specialized pools.
 *
 * Three pools with distinct policies share one elastic budget donated from
 * the buddy PMM (static quotas in phase 1):
 *
 *   POOL_WEIGHTS  model weights, linear streaming. LRU + frequency
 *                 tie-break + sequential prefetch. Read-mostly; dirty
 *                 pages (training passes) write back on eviction.
 *   POOL_KV       attention/KV states. Arena allocator, append-only,
 *                 NEVER auto-evicted: sessions own their extent until
 *                 explicitly ended (real-vLLM semantics: KV OOM is
 *                 reported, not silently dropped).
 *   POOL_SCRATCH  workspace temporaries. FIFO ring; wholesale recycle;
 *                 dirty contents write back when the ring wraps.
 */
#ifndef AIK_HMM_H
#define AIK_HMM_H

#include "kernel.h"

#define HMM_PAGE_SIZE   4096u

typedef enum {
    POOL_WEIGHTS = 0,
    POOL_KV      = 1,
    POOL_SCRATCH = 2,
    POOL_COUNT   = 3
} hmm_pool_t;

typedef struct hmm_page {
    struct hmm_page *lru_prev, *lru_next;  /* per-pool LRU/FIFO list */
    struct hmm_page *hash_next;            /* global bucket chain */
    u64 host_addr;
    u64 vram_addr;
    u32 model_id, page_idx;
    u8  pool        : 2;                   /* hmm_pool_t */
    u8  freq        : 2;
    u8  dirty       : 1;
    u8  resident    : 1;
} hmm_page_t;

typedef struct {
    const char *name;
    u64     host_base;
    u64     npages;
    u8      pool;
} hmm_model_t;

int  hmm_init(u64 initial_pool_pages, u64 max_pool_pages);
/* register into a specific pool */
int  hmm_register_model_p(const char *name, void *host_buf, u64 npages, hmm_pool_t pool);
/* convenience: weights pool */
int  hmm_register_model(const char *name, void *host_buf, u64 npages);

u64  hmm_fault(u32 model, u32 idx);          /* read path  -> model's pool */
u64  hmm_write_fault(u32 model, u32 idx);    /* write path -> dirty + freq boost */

/* KV-cache sessions (POOL_KV): extent owned until ended */
int  kv_session_begin(void);                 /* returns session id or -1 */
u64  kv_session_alloc(int sid, u32 npages);  /* first vram addr or 0 (KV OOM) */
void kv_session_end(int sid);                /* release extent */

/* Scratch ring (POOL_SCRATCH): cycling workspace slot */
u64  scratch_next(u32 words, int *rotated);

u64  hmm_restore_to_pmm(u32 target_pages);
void hmm_stats(void);
u64  hmm_amplification_x100(void);
u64  hmm_pool_pages(void);

#endif
