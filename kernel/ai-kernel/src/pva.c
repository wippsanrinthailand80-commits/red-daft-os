#include "pva.h"

/* ---------------------------------------------------------------------------
 * Paravirtual AI accelerator + Heterogeneous Memory Manager (HMM)
 *
 * This is the core IP of the kernel. We model a GPU-like device: it has a
 * small pool of "device memory" (dev_page_ptr[]) that acts as a CACHE over a
 * larger host-resident model (model_host[]). An "inference engine" accesses
 * model pages through pva_read(); if the page is not resident in device
 * memory, a page fault is raised, the HMM services it (evict LRU + migrate
 * host->device), and execution resumes transparently.
 *
 * This is exactly how NVIDIA UVM / AMD GPUVM demand-paging works, but the
 * policy (admission, eviction, KV-awareness) lives in the OS, so ANY ML
 * framework benefits. The fault handler is the hot path and is single-
 * threaded here; a real port needs fine-grained locking + IOMMU binding.
 * ------------------------------------------------------------------------- */

static u8 *dev_page_ptr[DEV_PAGES];     /* device "VRAM" pages (the cache) */
static u8 *model_host[MODEL_PAGES];     /* host backing store (the model) */
static s64 resident_dev[MODEL_PAGES];   /* dev page holding model page i, or -1 */
static s64 holds_model[DEV_PAGES];      /* model page in dev page j, or -1 */
static int dirty[DEV_PAGES];
static u64 lru_ts[DEV_PAGES];
static u64 tick;
static u32 faults, evicts;

void pva_init(void){
    for (int i = 0; i < DEV_PAGES;   i++){
        dev_page_ptr[i] = (u8 *)pmm_alloc();
        holds_model[i]  = -1; lru_ts[i] = 0; dirty[i] = 0;
    }
    for (int i = 0; i < MODEL_PAGES; i++){
        model_host[i]  = (u8 *)pmm_alloc();
        resident_dev[i] = -1;
    }
    tick = 0; faults = 0; evicts = 0;
}

u8 *pva_model_page(u32 pi){ return model_host[pi]; }

static int find_free_dev(void){
    for (int j = 0; j < DEV_PAGES; j++)
        if (holds_model[j] == -1) return j;
    return -1;
}

static int pick_victim(void){
    int v = 0;
    for (int j = 1; j < DEV_PAGES; j++)
        if (lru_ts[j] < lru_ts[v]) v = j;
    return v;
}

/* The fault handler: bring model page `page_i` into device memory.
 *  - if a free device page exists, use it (no eviction)
 *  - else evict the LRU device page (write back if dirty), then migrate */
static void pva_fault(u32 page_i){
    faults++;
    int j = find_free_dev();
    if (j < 0){
        j = pick_victim();
        s64 victim = holds_model[j];
        if (victim >= 0){
            if (dirty[j])
                for (int k = 0; k < PAGE_SIZE; k++)
                    model_host[victim][k] = dev_page_ptr[j][k];
            resident_dev[victim] = -1;
        }
        evicts++;
    }
    /* migrate host -> device */
    for (int k = 0; k < PAGE_SIZE; k++)
        dev_page_ptr[j][k] = model_host[page_i][k];
    holds_model[j]   = (s64)page_i;
    resident_dev[page_i] = (s64)j;
    dirty[j] = 0;
    lru_ts[j] = tick++;
}

/* Read one int16 weight at (page, elem), faulting it in if needed. */
static s16 pva_read(u32 page_i, u32 elem){
    if (resident_dev[page_i] < 0) pva_fault(page_i);
    int j = (int)resident_dev[page_i];
    lru_ts[j] = tick++;                  /* touch -> refresh LRU */
    u16 *p = (u16 *)dev_page_ptr[j];
    return (s16)p[elem];
}

/* "Inference engine": run op() over the whole model through the HMM.
 * Sequential access over 4x the device capacity => heavy, correct paging. */
s64 pva_run_inference(s32 (*op)(s16)){
    s64 acc = 0;
    for (u32 pi = 0; pi < MODEL_PAGES; pi++)
        for (u32 e = 0; e < ELEMS_PER_PAGE; e++)
            acc += (s64)op(pva_read(pi, e));
    return acc;
}

u32 pva_get_faults(void){ return faults; }
u32 pva_get_evicts(void){ return evicts; }
