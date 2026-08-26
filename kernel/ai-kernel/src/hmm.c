/* hmm.c — Heterogeneous Memory Manager v3: ten specialized pools.
 *
 *   POOL_WEIGHTS      LRU + freq + prefetch (streaming inference)
 *   POOL_KV           arena sessions, never auto-evicted
 *   POOL_SCRATCH      FIFO ring, wholesale recycle
 *   POOL_ACTIVATIONS  LRU
 *   POOL_EMBED        LRU (+prefetch on hit)
 *   POOL_ATTENTION    LRU
 *   POOL_WORKSPACE    FIFO
 *   POOL_CACHE        LRU
 *   POOL_TENSOR       LRU
 *   POOL_GENERIC      LRU (fallback / user-defined)
 *
 * Ten pools share one elastic budget (phase-1 static quotas, ~10% each)
 * donated run-by-run from the buddy PMM. Kernel 2 (AI-Kernel) exposes
 * all 10 via hmm_register_model_p(pool=0..9).
 *
 * Locking: one spinlock guards all pool/hashtable/list state; compute paths
 * hold it only during fault resolution. */
#include "hmm.h"

#define MAX_MODELS 16
#define MAX_RUNS    8            /* per pool */
#define MAX_SLOTS 4096           /* slab + freelist capacity */
#define HT_BITS    10

typedef struct { u64 base, pages, used; int order; } run_t;

typedef struct {
    u64 quota_pages;
    u64 cur_pages;
    run_t runs[MAX_RUNS]; u32 nruns;
    u64 freelist[MAX_SLOTS]; u32 nfree;
    /* policy list (LRU for weights, FIFO/ring for scratch, extent chain for kv) */
    hmm_page_t *head, *tail;
    u64 evictions, hits, prefetched, writebacks;
} pool_t;

static struct {
    spinlock_t lock;
    hmm_page_t *htab[1u<<HT_BITS];
    u64 faults, misses, restored_ever, bytes_migrated;
} G;

static hmm_model_t models[MAX_MODELS];
static u32 nmodels;
static pool_t PL[POOL_COUNT];
static u64 cap_pages;

static hmm_page_t page_slab[MAX_SLOTS];
static u8          slab_used[MAX_SLOTS];

/* --------------------------- slab ---------------------------- */
static hmm_page_t *page_alloc(void){
    for(u32 i=0;i<MAX_SLOTS;i++)
        if(!slab_used[i]){ slab_used[i]=1; return &page_slab[i]; }
    return NULL;
}
static void page_free(hmm_page_t *p){
    u32 i=(u32)(p-page_slab);
    if(i<MAX_SLOTS) slab_used[i]=0;
}

/* --------------------------- hash ---------------------------- */
static u32 hsh(u32 m,u32 i){ return ((m*2654435761u)^(i*97u))&((1u<<HT_BITS)-1); }
static hmm_page_t *ht_find(u32 m,u32 i){
    hmm_page_t *p=G.htab[hsh(m,i)];
    while(p){ if(p->model_id==m&&p->page_idx==i) return p; p=p->hash_next; }
    return NULL;
}
static void ht_insert(hmm_page_t*p){
    u32 h=hsh(p->model_id,p->page_idx);
    p->hash_next=G.htab[h]; G.htab[h]=p;
}
static void ht_remove(hmm_page_t*p){
    hmm_page_t**c=&G.htab[hsh(p->model_id,p->page_idx)];
    while(*c){ if(*c==p){ *c=p->hash_next; p->hash_next=NULL; return; } c=&(*c)->hash_next; }
}

/* ----------------------- per-pool slots ---------------------- */
static void slot_free(pool_t*L,u64 va){ if(va&&L->nfree<MAX_SLOTS) L->freelist[L->nfree++]=va; }
static u64  slot_alloc(pool_t*L){
    if(L->nfree) return L->freelist[--L->nfree];
    for(u32 r=0;r<L->nruns;r++)
        if(L->runs[r].used<L->runs[r].pages)
            return L->runs[r].base + L->runs[r].used++ * HMM_PAGE_SIZE;
    return 0;
}
static int pool_has_slot(pool_t*L){
    if(L->nfree) return 1;
    for(u32 r=0;r<L->nruns;r++) if(L->runs[r].used<L->runs[r].pages) return 1;
    return 0;
}
static int pool_grow(hmm_pool_t pid){
    pool_t*L=&PL[pid];
    if(L->nruns>=MAX_RUNS || L->cur_pages>=L->quota_pages) return -1;
    u64 chunk=128;
    if(chunk>L->quota_pages-L->cur_pages) chunk=L->quota_pages-L->cur_pages;
    int order=0; while((1u<<order)<chunk && order<10) order++;
    u64 b=pmm_donate_range((u32)chunk);
    if(!b) return -1;
    L->runs[L->nruns++]=(run_t){ b, chunk, 0, order };
    L->cur_pages+=chunk;
    kprintf("[hmm] pool %u grew +%llu pages -> %llu\n",pid,chunk,L->cur_pages);
    return 0;
}

/* --------------------------- lists --------------------------- */
static void push_front(pool_t*L,hmm_page_t*p){
    p->lru_next=L->head; p->lru_prev=NULL;
    if(L->head) L->head->lru_prev=p; else L->tail=p;
    L->head=p;
}
static void push_back(pool_t*L,hmm_page_t*p){
    p->lru_prev=L->tail; p->lru_next=NULL;
    if(L->tail) L->tail->lru_next=p; else L->head=p;
    L->tail=p;
}
static void unlink_page(pool_t*L,hmm_page_t*p){
    if(p->lru_prev)p->lru_prev->lru_next=p->lru_next; else L->head=p->lru_next;
    if(p->lru_next)p->lru_next->lru_prev=p->lru_prev; else L->tail=p->lru_prev;
    p->lru_prev=p->lru_next=NULL;
}

/* -------------------------- policies ------------------------- */
static void page_writeback(hmm_page_t*p){
    if(!p->dirty||!p->host_addr) return;
    kmemcpy((void*)(usize)p->host_addr,(void*)(usize)p->vram_addr,HMM_PAGE_SIZE);
    p->dirty=0; PL[p->pool].writebacks++;
}
static int evict_weights(void){
    pool_t*L=&PL[POOL_WEIGHTS];
    hmm_page_t*v=L->tail; int guard=0;
    while(v && v!=L->head && guard++<64 && v->freq>=2){
        v->freq--;                       /* second chance: decay+promote */
        unlink_page(L,v); push_front(L,v); v=L->tail;
    }
    if(!v) return -1;
    page_writeback(v);
    ht_remove(v);
    unlink_page(L,v);
    slot_free(L,v->vram_addr);
    v->vram_addr=0;
    L->evictions++; G.bytes_migrated+=HMM_PAGE_SIZE;
    page_free(v);
    return 0;
}
static int evict_fifo(hmm_pool_t pid){
    pool_t*L=&PL[pid];
    hmm_page_t*v=L->tail;                /* FIFO: oldest */
    if(!v) return -1;
    page_writeback(v);
    ht_remove(v);
    unlink_page(L,v);
    slot_free(L,v->vram_addr);
    v->vram_addr=0;
    L->evictions++;
    page_free(v);
    return 0;
}
static int evict_scratch(void){ return evict_fifo(POOL_SCRATCH); }
static int evict_lru(hmm_pool_t pid){
    pool_t*L=&PL[pid];
    hmm_page_t*v=L->tail;
    if(!v) return -1;
    page_writeback(v);
    ht_remove(v);
    unlink_page(L,v);
    slot_free(L,v->vram_addr);
    v->vram_addr=0;
    L->evictions++; G.bytes_migrated+=HMM_PAGE_SIZE;
    page_free(v);
    return 0;
}
static int ensure_capacity(hmm_pool_t pid){
    pool_t*L=&PL[pid];
    if(pool_has_slot(L)) return 0;
    if(pool_grow(pid)==0) return 0;      /* every pool grows to its quota */
    if(pid==POOL_WEIGHTS) return evict_weights();
    if(pid==POOL_SCRATCH || pid==POOL_WORKSPACE) return evict_fifo(pid);
    if(pid==POOL_KV) return -1;          /* KV: explicit OOM, never evicts */
    return evict_lru(pid);               /* all other pools: generic LRU */
}

/* ------------------------- load path ------------------------- */
static hmm_page_t *load_new(u32 m,u32 idx,int is_write){
    hmm_pool_t pid=models[m].pool;
    if(ensure_capacity(pid)!=0) return NULL;
    u64 va=slot_alloc(&PL[pid]);
    hmm_page_t*q=page_alloc();
    if(!q||!va){ if(q)page_free(q); return NULL; }
    q->model_id=m;q->page_idx=idx;
    q->host_addr=models[m].host_base ? models[m].host_base+(u64)idx*HMM_PAGE_SIZE : 0;
    q->vram_addr=va; q->pool=(u8)pid;
    q->freq=is_write?3:1; q->dirty=is_write?1:0; q->resident=1;
    if(q->host_addr){
        kmemcpy((void*)(usize)va,(void*)(usize)q->host_addr,HMM_PAGE_SIZE);
        { const u64*s=(const u64*)(usize)q->host_addr; const u64*d=(const u64*)(usize)va;
          for(u32 k=0;k<HMM_PAGE_SIZE/8;k++) if(s[k]!=d[k]){ dbgmark('X'); break; } }
    }
    ht_insert(q);
    if(pid==POOL_KV) push_back(&PL[pid],q); else push_front(&PL[pid],q);
    return q;
}
static u64 fault_in(u32 m,u32 idx,int is_write){
    if(m>=nmodels||idx>=models[m].npages) return 0;
    spin_lock(&G.lock);
    G.faults++;
    hmm_page_t*p=ht_find(m,idx);
    if(p){
        PL[p->pool].hits++;
        if(p->freq<3)p->freq++;
        if(is_write)p->dirty=1;
        spin_unlock(&G.lock);
        return p->vram_addr;
    }
    G.misses++;
    p=load_new(m,idx,is_write);
    spin_unlock(&G.lock);
    if(!p) return 0;
    if(models[m].pool==POOL_WEIGHTS){     /* sequential prefetch window */
        for(u32 k=1;k<=3;k++){
            u32 pi=idx+k; if(pi>=models[m].npages)break;
            spin_lock(&G.lock);
            if(ht_find(m,pi)){spin_unlock(&G.lock);continue;}
            if(load_new(m,pi,0)) PL[POOL_WEIGHTS].prefetched++;
            spin_unlock(&G.lock);
        }
    }
    return p->vram_addr;
}

/* ---------------------------- API ---------------------------- */
int hmm_init(u64 initial_pool_pages, u64 max_pool_pages){
    kmemset(&G,0,sizeof(G)); kmemset(models,0,sizeof(models));
    kmemset(PL,0,sizeof(PL)); kmemset(page_slab,0,sizeof(page_slab));
    nmodels=0; cap_pages=max_pool_pages;
    /* 10 pools: weighted quotas so the demo still fits (kv 64 needs >=64)
     * weights 30% / kv 20% / scratch 10% / remaining 7 pools share 40% */
    u64 w = max_pool_pages * 30 / 100;
    u64 k = max_pool_pages * 20 / 100;
    u64 s = max_pool_pages * 10 / 100;
    u64 rem = max_pool_pages - w - k - s;
    PL[POOL_WEIGHTS].quota_pages     = w;
    PL[POOL_KV].quota_pages          = k;
    PL[POOL_SCRATCH].quota_pages     = s;
    u64 base = rem / 7;
    u64 r2   = rem % 7;
    for(u32 i=3;i<POOL_COUNT;i++) PL[i].quota_pages = base + (i-3 < r2 ? 1 : 0);
    /* ensure at least 1 page per pool for tiny max values */
    for(u32 i=0;i<POOL_COUNT;i++) if(!PL[i].quota_pages) PL[i].quota_pages=1;
    u64 first=initial_pool_pages;
    if(first>PL[POOL_WEIGHTS].quota_pages) first=PL[POOL_WEIGHTS].quota_pages;
    u64 b=pmm_donate_range((u32)first);
    if(!b){kprintf("[hmm] initial donation failed\n");return -1;}
    int o=0;while((1u<<o)<first&&o<10)o++;
    PL[POOL_WEIGHTS].runs[PL[POOL_WEIGHTS].nruns++]=(run_t){b,first,0,o};
    PL[POOL_WEIGHTS].cur_pages=first;
    kprintf("[hmm] v3 10 pools: quotas=");
    for(u32 i=0;i<POOL_COUNT;i++) kprintf("%lluK%s", PL[i].quota_pages*4, i+1<POOL_COUNT?",":"");
    kprintf(" | free RAM %lluK\n", pmm_free_pages()*4);
    return 0;
}
int hmm_register_model_p(const char*n,void*buf,u64 np,hmm_pool_t pool){
    if(nmodels>=MAX_MODELS||pool>=POOL_COUNT)return -1;
    models[nmodels]=(hmm_model_t){n,(u64)(usize)buf,np,(u8)pool};
    return nmodels++;
}
int hmm_register_model(const char*n,void*b,u64 np){return hmm_register_model_p(n,b,np,POOL_WEIGHTS);}
int hmm_model_id(const char*n){for(u32 i=0;i<nmodels;i++)if(!kstrcmp(models[i].name,n))return i;return -1;}
u64 hmm_fault(u32 m,u32 idx){return fault_in(m,idx,0);}
u64 hmm_write_fault(u32 m,u32 idx){return fault_in(m,idx,1);}

/* ---- KV sessions ---- */
typedef struct{ int sid; hmm_page_t *first,*last; u32 npages; } kvsess_t;
static kvsess_t sess[8];
static int next_sid=1;
int kv_session_begin(void){
    for(int i=0;i<8;i++) if(sess[i].sid==0){ sess[i].sid=next_sid; return next_sid++; }
    return -1;
}
u64 kv_session_alloc(int sid,u32 np){
    u64 have=0;
    for(int i=0;i<8;i++) if(sess[i].sid) have+=sess[i].npages;
    if(have+(u64)np > PL[POOL_KV].quota_pages) return 0;    /* KV OOM */
    kvsess_t*S=NULL; for(int i=0;i<8;i++) if(sess[i].sid==sid) S=&sess[i];
    if(!S || S->first || S->npages) return 0;
    u64 first_va=0;
    for(u32 k=0;k<np;k++){
        if(ensure_capacity(POOL_KV)!=0) goto rollback;
        u64 va=slot_alloc(&PL[POOL_KV]);
        if(!va) goto rollback;
        hmm_page_t*q=page_alloc();
        if(!q){ slot_free(&PL[POOL_KV],va); goto rollback; }
        q->model_id=0x7F000000u|(u32)sid; q->page_idx=k;
        q->host_addr=0; q->vram_addr=va; q->pool=POOL_KV;
        q->freq=3; q->dirty=1; q->resident=1;
        ht_insert(q);
        push_back(&PL[POOL_KV],q);
        if(S->last){ S->last->lru_next=q; q->lru_prev=S->last; S->last=q; }
        else { S->first=S->last=q; q->lru_prev=NULL; }
        if(k==0) first_va=va;
        continue;
rollback:
        /* atomic: undo this session's pages so far, report OOM honestly */
        {
            hmm_page_t*i2=S->first;
            while(i2){
                hmm_page_t*n=i2->lru_next;
                ht_remove(i2); unlink_page(&PL[POOL_KV],i2);
                slot_free(&PL[POOL_KV],i2->vram_addr);
                page_free(i2); i2=n;
            }
            S->first=S->last=NULL; S->npages=0;
        }
        return 0;
    }
    S->npages=np;
    return first_va;
}
void kv_session_end(int sid){
    for(int i=0;i<8;i++) if(sess[i].sid==sid){
        hmm_page_t*p=sess[i].first;
        while(p){
            hmm_page_t*n=p->lru_next;
            ht_remove(p);
            unlink_page(&PL[POOL_KV],p);
            slot_free(&PL[POOL_KV],p->vram_addr);
            page_free(p);
            p=n;
        }
        sess[i]=(kvsess_t){0,NULL,NULL,0};
        return;
    }
}

/* ---- Scratch ring ---- */
static int scratch_mid=-1;               /* internal pseudo-model */
static u32 scratch_pos=0;
u64 scratch_next(u32 words,int*rotated){
    (void)words;
    int rot=0;
    if(PL[POOL_SCRATCH].cur_pages >= PL[POOL_SCRATCH].quota_pages &&
       !pool_has_slot(&PL[POOL_SCRATCH])){ rot=1; evict_scratch(); }
    if(scratch_mid<0)
        scratch_mid=hmm_register_model_p("__scratch",0,0xFFFF,POOL_SCRATCH);
    hmm_page_t*q=load_new((u32)scratch_mid,scratch_pos,0);
    scratch_pos++;
    if(rotated)*rotated=rot;
    return q?q->vram_addr:0;
}

/* ----------------------- restore/stats ----------------------- */
static u64 pool_total(void){
    u64 t=0; for(u32 p=0;p<POOL_COUNT;p++) t+=PL[p].cur_pages; return t;
}
u64 hmm_restore_to_pmm(u32 target_pages){
    u64 freed=0;
    for(int pi=POOL_COUNT-1;pi>=0 && freed<(u64)target_pages;pi--){
        pool_t*L=&PL[pi];
        spin_lock(&G.lock);
        while(freed<(u64)target_pages && L->nruns>1){
            run_t*r=&L->runs[L->nruns-1];
            int again=1;
            while(again){
                again=0;
                for(u32 h=0;h<(1u<<HT_BITS)&&!again;h++){
                    hmm_page_t*p=G.htab[h];
                    while(p){
                        if(p->pool==(u8)pi &&
                           p->vram_addr>=r->base &&
                           p->vram_addr< r->base+r->pages*HMM_PAGE_SIZE){
                            page_writeback(p); ht_remove(p);
                            unlink_page(L,p); page_free(p);
                            again=1; break;
                        }
                        p=p->hash_next;
                    }
                }
            }
            pmm_restore_block(r->base,r->order,(u32)r->pages);
            L->cur_pages-=r->pages; freed+=r->pages;
            G.restored_ever+=r->pages;
            L->nruns--;
        }
        spin_unlock(&G.lock);
    }
    return freed;
}
void hmm_stats(void){
    static const char*nm[POOL_COUNT]={"weights","kv","scratch","activ","embed","attn","worksp","cache","tensor","generic"};
    u64 mb=0; for(u32 i=0;i<nmodels;i++)
        if(models[i].name && models[i].name[0]!='_') mb+=models[i].npages*HMM_PAGE_SIZE;
    for(u32 p=0;p<POOL_COUNT;p++)
        kprintf("[hmm] %-8s cur=%lluKB quota=%lluKB evict=%llu hit=%llu wb=%llu\n",
            nm[p],PL[p].cur_pages*4,PL[p].quota_pages*4,
            PL[p].evictions,PL[p].hits,PL[p].writebacks);
    kprintf("[hmm] faults=%llu misses=%llu migrated=%lluKB restored=%lluKB\n",
        G.faults,G.misses,G.bytes_migrated/1024,G.restored_ever*4);
    kprintf("[hmm] models=%lluKB amplification=%llux\n",
        mb/1024, pool_total()? (mb/1024)/(pool_total()*4):0);
}
u64 hmm_amplification_x100(void){
    u64 mp=0; for(u32 i=0;i<nmodels;i++)
        if(models[i].name&&models[i].name[0]!='_') mp+=models[i].npages;
    u64 pp=pool_total();
    return pp?(mp*100)/pp:0;
}
u64 hmm_pool_pages(void){ return pool_total(); }
