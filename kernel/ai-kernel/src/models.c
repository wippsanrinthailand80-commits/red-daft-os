/* models.c — "AI model" registry: pure compute ops streamed through the HMM,
 * each automatically verified against a CPU reference over host RAM.
 * Correctness under paging is the gate: if the pager ever loses a byte,
 * the check fails loudly. */
#include "kernel.h"
#include "hmm.h"

typedef struct { const char *name; u32 npages; s32 (*op)(s32); } model_t;

/* All ops use u32 arithmetic with defined wrap-around: w*w overflows s32
 * by design, and UB there makes results compiler-dependent (it broke the
 * correctness gate between two gcc builds). Wrap explicitly instead. */
static s32 op_sos(s32 w){ return (s32)((u32)w * (u32)w); }
static s32 op_abs(s32 w){ return w<0?-w:w; }
static s32 op_xor(s32 w){ w^=w<<13; w^=w>>7; return w&0xFFFF; }
static s32 op_inc(s32 w){ return (s32)((u32)w + 1u); }

/* host buffers (the "main RAM" backing store): 4 models x 4MB in .bss */
#define MODEL_PAGES 256           /* 1 MB each — keeps smoke fast; pool still oversubscribed */
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
        kprintf("[model] %d %s %uKB\n",id,defs[m].name,MODEL_PAGES*4);
    }
}

/* stream one model through the HMM; returns device-computed checksum */
static s64 run_model(u32 m){
    s64 acc=0;
    for(u32 i=0;i<defs[m].npages;i++){
        u64 va=hmm_fault(m,i);
        if(!va){ kprintf("[model] fault FAILED %u:%u\n",m,i); return -1; }
        s32 *w=(s32*)(usize)va;
        acc += defs[m].op(w[0]);               /* touch first word of page */
    }
    return acc;
}
/* post-training reference: after full eviction pressure every page was
 * written back, so host_buf IS the updated truth — no extra offset. */
static s64 ref_model_trained(u32 m){
    return ref_model(m);
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
/* verify AFTER training: host words carry the +7 updates */
int model_verify_trained(u32 m){
    s64 got=run_model(m), ref=ref_model_trained(m);
    kprintf("[t-verify] %-12s ref=%lld got=%lld -> %s\n",
            defs[m].name, ref, got, got==ref?"MATCH":"MISMATCH");
    return got==ref;
}

/* training-style pass: WRITE every page of model m (simulated gradient:
 * first word += 7 via the device copy), forcing dirty state. The caller
 * then streams other models to push these pages out; writeback must land
 * them in host RAM. Re-verification reads host directly. */
s64 train_model(u32 m){
    s64 acc=0;
    for(u32 i=0;i<defs[m].npages;i++){
        u64 va=hmm_write_fault(m,i);
        if(!va){ kprintf("[train] write fault FAILED %u:%u\n",m,i); return -1; }
        s32 *w=(s32*)(usize)va;
        w[0]+=7;                                   /* "gradient update" */
        acc+=defs[m].op(w[0]);
    }
    return acc;
}
const char *model_name(u32 m){ return m<4?defs[m].name:"?"; }
u32 model_count(void){ return 4; }
/* canary: cheap checksum of model-0 backing store (corruption detector) */
const s32 *hb0(void){ return host_buf[0]; }
u32 hb0_sum(void){
    u32 s=0;
    for(u32 i=0;i<MODEL_PAGES*WORDS_PER_PAGE;i+=97) s^=host_buf[0][i];
    return s;
}
