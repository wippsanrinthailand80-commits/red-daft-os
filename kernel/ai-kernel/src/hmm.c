/* hmm.c — Heterogeneous Memory Manager v2.
 * Small elastic VRAM pool cached over big host-RAM models.
 *   - pool grows by DONATING contiguous runs from the buddy PMM
 *   - shrinks by RESTORING runs back to it ("RAM from the main kernel")
 *   - LRU eviction with frequency tie-break (2-bit counters)
 *   - sequential prefetch window, dirty writeback, live stats
 *
 * Locking: one spinlock guards all pool/hashtable/LRU state. The compute
 * paths take the lock only during fault resolution, never while crunching.
 */
#include "hmm.h"

#define MAX_MODELS 16
#define MAX_RUNS    8

typedef struct { u64 base, pages, used; int order; } run_t;

static hmm_t       H;
static hmm_model_t models[MAX_MODELS];
static u32         nmodels;
static run_t       runs[MAX_RUNS];
static u32         nruns;
static u64         cap_pages;

/* ------------------------------------------------------------------ */
static u32 hash(u32 m,u32 i){ return ((m*2654435761u) ^ (i*97u)) & ((1u<<HMM_HASH_BITS)-1); }

static hmm_page_t *ht_find(u32 m,u32 idx){
    hmm_page_t *p = H.htab[hash(m,idx)];
    while(p){
        if(p->model_id==m && p->page_idx==idx) return p;
        p = p->hash_next;
    }
    return NULL;
}
static void ht_insert(hmm_page_t *p){
    u32 h = hash(p->model_id,p->page_idx);
    p->hash_next = H.htab[h];                   /* push front of bucket */
    H.htab[h]    = p;
}
static void ht_remove(hmm_page_t *p){
    hmm_page_t **cur = &H.htab[hash(p->model_id,p->page_idx)];
    while(*cur){
        if(*cur==p){ *cur=p->hash_next; p->hash_next=NULL; return; }
        cur = &(*cur)->hash_next;
    }
}

static void lru_push_mru(hmm_page_t *p){
    p->lru_next=H.lru_head; p->lru_prev=NULL;
    if(H.lru_head) H.lru_head->lru_prev=p; else H.lru_tail=p;
    H.lru_head=p;
}
static void lru_unlink(hmm_page_t *p){
    if(p->lru_prev) p->lru_prev->lru_next=p->lru_next; else H.lru_head=p->lru_next;
    if(p->lru_next) p->lru_next->lru_prev=p->lru_prev; else H.lru_tail=p->lru_prev;
    p->lru_prev=p->lru_next=NULL;
}

/* --------------------- page-metadata slab --------------------------
 * Fixed-size allocation from a static pool: immune to heap fragmentation
 * under the heavy alloc/free churn of streaming eviction. */
#define MAX_SLOTS 4096
static hmm_page_t page_slab[MAX_SLOTS];
static u8          slab_used[MAX_SLOTS];
static u32         slab_inuse;

static hmm_page_t *page_alloc(void){
    for(u32 i=0;i<MAX_SLOTS;i++)
        if(!slab_used[i]){ slab_used[i]=1; slab_inuse++; return &page_slab[i]; }
    return NULL;
}
static void page_free(hmm_page_t *p){
    u32 idx = (u32)(p - page_slab);
    if(idx<MAX_SLOTS && slab_used[idx]){ slab_used[idx]=0; slab_inuse--; }
}

/* ------------------------- elastic pool --------------------------- */
static u64 slot_freelist[MAX_SLOTS];
static u32 nfree_slots;

static u64 pool_pages_now(void){ u64 t=0; for(u32 r=0;r<nruns;r++) t+=runs[r].pages; return t; }

static void slot_free(u64 va){
    if(va && nfree_slots<MAX_SLOTS) slot_freelist[nfree_slots++]=va;
}
static u64 slot_alloc(void){
    if(nfree_slots) return slot_freelist[--nfree_slots];   /* recycle first */
    for(u32 r=0;r<nruns;r++)
        if(runs[r].used < runs[r].pages)
            return runs[r].base + runs[r].used++ * HMM_PAGE_SIZE;
    return 0;
}
static int pool_has_slot(void){
    for(u32 r=0;r<nruns;r++) if(runs[r].used<runs[r].pages) return 1;
    return 0;
}
static int pool_grow(void){
    if(nruns>=MAX_RUNS || pool_pages_now()>=cap_pages) return -1;
    u64 chunk = 128;
    u64 have = pool_pages_now();
    if(chunk > cap_pages-have) chunk = cap_pages-have;
    int order=0; while((1u<<order)<chunk && order<10) order++;
    u64 b = pmm_donate_range((u32)chunk);
    if(!b) return -1;
    runs[nruns++] = (run_t){ b, chunk, 0, order };
    H.pool_pages += chunk;
    kprintf("[hmm] PMM donated %llu pages -> pool=%llu pages\n", chunk, pool_pages_now());
    return 0;
}

/* --------------------------- eviction ----------------------------- */
static void page_writeback(hmm_page_t *p){
    if(!p->dirty) return;
    dbgmark('W');
    kmemcpy((void*)(usize)p->host_addr,(void*)(usize)p->vram_addr,HMM_PAGE_SIZE);
    p->dirty=0; H.writebacks++;
}
static int evict_one(void){
    hmm_page_t *v=H.lru_tail;
    int guard=0;
    while(v && v!=H.lru_head && guard++ < 64 && v->freq>=2){
        v->freq--;                       /* second chance: decay + promote */
        lru_unlink(v); lru_push_mru(v);
        v=H.lru_tail;
    }
    if(!v) return -1;
    dbgmark('E');
    page_writeback(v);
    ht_remove(v);
    if(v==H.lru_head && v==H.lru_tail){ H.lru_head=H.lru_tail=NULL; }
    else lru_unlink(v);
    slot_free(v->vram_addr);          /* recycle the device slot */
    H.evictions++;
    H.bytes_migrated += HMM_PAGE_SIZE;
    page_free(v);
    return 0;
}

static int ensure_capacity(void){
    if(pool_has_slot()) return 0;
    if(pool_grow()==0)   return 0;
    return evict_one();
}

/* resident-load without policy side effects (used by prefetch too) */
static hmm_page_t *load_new(u32 m,u32 idx,int is_write){
    if(ensure_capacity()!=0) return NULL;
    hmm_model_t *mo=&models[m];
    u64 va = slot_alloc();
    hmm_page_t *q = page_alloc();
    if(!q || !va){ if(q) page_free(q); return NULL; }
    q->model_id=m; q->page_idx=idx;
    q->host_addr = mo->host_base + (u64)idx*HMM_PAGE_SIZE;
    q->vram_addr = va;
    q->freq      = is_write?3:1;
    q->dirty     = is_write?1:0;
    q->resident  = 1;
    kmemcpy((void*)(usize)va,(void*)(usize)q->host_addr,HMM_PAGE_SIZE);
    /* migration integrity check: verify the copy landed intact */
    {
        const u64 *src=(const u64*)(usize)q->host_addr;
        const u64 *dst=(const u64*)(usize)va;
        for(u32 k=0;k<HMM_PAGE_SIZE/8;k++){
            if(src[k]!=dst[k]){ dbgmark('X'); break; }
        }
    }
    H.bytes_migrated += HMM_PAGE_SIZE;
    ht_insert(q);
    lru_push_mru(q);
    return q;
}

static u64 fault_in(u32 m,u32 idx,int is_write){
    if(m>=nmodels || idx>=models[m].npages) return 0;
    spin_lock(&H.lock);
    H.faults++;
    hmm_page_t *p = ht_find(m,idx);
    if(p){
        H.hits++;
        if(p->freq<3) p->freq++;
        if(is_write) p->dirty=1;
        spin_unlock(&H.lock);
        return p->vram_addr;
    }
    H.misses++;
    p = load_new(m,idx,is_write);
    spin_unlock(&H.lock);
    if(!p) return 0;

    /* sequential prefetch: weights stream linearly, pull next 3 */
    for(u32 k=1;k<=3;k++){
        u32 pi=idx+k;
        if(pi>=models[m].npages) break;
        spin_lock(&H.lock);
        if(ht_find(m,pi)){ spin_unlock(&H.lock); continue; }
        if(load_new(m,pi,0)) H.prefetched++;
        spin_unlock(&H.lock);
    }
    return p->vram_addr;
}

/* ------------------------------ API -------------------------------- */
int hmm_init(u64 initial_pool_pages, u64 max_pool_pages){
    kmemset(&H,0,sizeof(H)); kmemset(models,0,sizeof(models));
    kmemset(runs,0,sizeof(runs));
    nmodels=0; nruns=0; cap_pages=max_pool_pages;
    u64 b=pmm_donate_range((u32)initial_pool_pages);
    if(!b){ kprintf("[hmm] initial donation failed\n"); return -1; }
    int iorder=0; while((1u<<iorder)<initial_pool_pages && iorder<10) iorder++;
    runs[nruns++] = (run_t){ b, initial_pool_pages, 0, iorder };
    H.pool_base=b; H.pool_pages=initial_pool_pages;
    kprintf("[hmm] pool=%lluKB cap=%lluKB | free RAM %lluKB\n",
            initial_pool_pages*4, max_pool_pages*4, pmm_free_pages()*4);
    return 0;
}
int hmm_register_model(const char *name,void *buf,u64 npages){
    if(nmodels>=MAX_MODELS) return -1;
    models[nmodels]=(hmm_model_t){ name,(u64)(usize)buf,npages };
    return nmodels++;
}
int hmm_model_id(const char *name){
    for(u32 i=0;i<nmodels;i++) if(!kstrcmp(models[i].name,name)) return i;
    return -1;
}
u64 hmm_fault(u32 m,u32 idx){ return fault_in(m,idx,0); }
u64 hmm_write_fault(u32 m,u32 idx){ return fault_in(m,idx,1); }

u64 hmm_restore_to_pmm(u32 target_pages){
    u64 freed=0;
    spin_lock(&H.lock);
    while(freed<target_pages && nruns>1){
        run_t *r=&runs[nruns-1];
        /* flush+drop every resident page inside this run */
        int again=1;
        while(again){
            again=0;
            for(u32 h=0; h<(1u<<HMM_HASH_BITS) && !again; h++){
                hmm_page_t *p=H.htab[h];
                while(p){
                    if(p->vram_addr>=r->base &&
                       p->vram_addr< r->base+r->pages*HMM_PAGE_SIZE){
                        page_writeback(p);
                        ht_remove(p);
                        if(p==H.lru_head||p==H.lru_tail||p->lru_prev||p->lru_next)
                            lru_unlink(p);
                        /* slot belongs to the run being dropped: drop, not recycle */
                        page_free(p);
                        again=1;
                        break;
                    }
                    p=p->hash_next;
                }
            }
        }
        kprintf("[hmm] restoring %llu pages to PMM\n", r->pages);
        pmm_restore_block(r->base, r->order, (u32)r->pages);
        H.pool_pages-=r->pages; freed+=r->pages; H.restored_ever+=r->pages;
        nruns--;
    }
    spin_unlock(&H.lock);
    return freed;
}

void hmm_stats(void){
    u64 mb=0; for(u32 i=0;i<nmodels;i++) mb+=models[i].npages*4;
    kprintf("[hmm] faults=%llu hits=%llu misses=%llu hit%%=%llu\n",
            H.faults,H.hits,H.misses, H.faults?(H.hits*100)/H.faults:0);
    kprintf("[hmm] evict=%llu writeback=%llu prefetched=%llu migrated=%lluKB\n",
            H.evictions,H.writebacks,H.prefetched,H.bytes_migrated/1024);
    kprintf("[hmm] pool=%lluKB | models=%lluKB | amplification=%llux | ram_restored=%lluKB\n",
            H.pool_pages*4, mb/1024,
            H.pool_pages? (mb/1024)/(H.pool_pages*4):0, H.restored_ever*4);
}
u64 hmm_amplification_x100(void){
    u64 mp=0; for(u32 i=0;i<nmodels;i++) mp+=models[i].npages;
    return H.pool_pages? (mp*100)/H.pool_pages : 0;
}
u64 hmm_pool_pages(void){ return pool_pages_now(); }
