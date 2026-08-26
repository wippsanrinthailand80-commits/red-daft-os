/* models.c — "AI model" registry: pure compute ops streamed through the HMM,
 * each automatically verified against a CPU reference over host RAM.
 * Correctness under paging is the gate: if the pager ever loses a byte,
 * the check fails loudly. */
#include "kernel.h"
#include "hmm.h"

typedef struct { const char *name; u32 npages; s32 (*op)(s32); } model_t;

static s32 op_sos(s32 w){ return w*w; }
static s32 op_abs(s32 w){ return w<0?-w:w; }
static s32 op_xor(s32 w){ w^=w<<13; w^=w>>7; return w&0xFFFF; }
static s32 op_inc(s32 w){ return w+1; }

/* host buffers (the "main RAM" backing store): 4 models x 4MB in .bss */
#define MODEL_PAGES 1024          /* 4 MB each */
#define WORDS_PER_PAGE 1024       /* 4096 / sizeof(s32) */
static s32 host_buf[4][MODEL_PAGES*WORDS_PER_PAGE];
static const model_t defs[4] = {
    { "sum-squares", MODEL_PAGES, op_sos },
    { "abs-sum",     MODEL_PAGES, op_abs },
    { "xor-mix",     MODEL_PAGES, op_xor },
    { "increment",   MODEL_PAGES, op_inc },
};

void models_init(void){
    u32 x=12345;
    for(int m=0;m<4;m++)
        for(u32 i=0;i<MODEL_PAGES*WORDS_PER_PAGE;i++){
            x^=x<<13; x^=x>>17; x^=x<<5;
            host_buf[m][i]=(s32)(x|1);
        }
    for(int m=0;m<4;m++){
        int id=hmm_register_model(defs[m].name,host_buf[m],MODEL_PAGES);
        kprintf("[model] %d '%s' %uKB\n",id,defs[m].name,MODEL_PAGES*4);
    }
}

/* stream one model through the HMM; returns device-computed checksum */
static s64 run_model(u32 m){
    s64 acc=0;
    for(u32 i=0;i<defs[m].npages;i++){
        u64 va = hmm_fault(m,i);
        if(!va){ kprintf("[model] fault FAILED %u:%u\n",m,i); return -1; }
        s32 *w = (s32*)(u32)va;
        acc += defs[m].op(w[0]);               /* touch first word of page */
    }
    return acc;
}
/* same computation straight over host RAM (ground truth) */
static s64 ref_model(u32 m){
    s64 acc=0; const s32 *w=host_buf[m];
    for(u32 i=0;i<defs[m].npages;i++) acc += defs[m].op(w[i*WORDS_PER_PAGE]);
    return acc;
}
int model_verify(u32 m){
    s64 got=run_model(m), ref=ref_model(m);
    kprintf("[verify] %-12s ref=%lld got=%lld -> %s\n",
            defs[m].name, ref, got, got==ref?"MATCH":"MISMATCH");
    return got==ref;
}
const char *model_name(u32 m){ return m<4?defs[m].name:"?"; }
u32 model_count(void){ return 4; }
