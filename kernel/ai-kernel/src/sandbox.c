#include "pva.h"

/* ---------------------------------------------------------------------------
 * The Sandbox: a registry where ANY "AI model" can be registered as a pure
 * compute op over the paged weight buffer, then verified for correctness
 * under constrained VRAM. The HMM is supposed to be transparent, so the
 * result computed through paged device memory must equal the CPU reference.
 *
 * To test a new model: write an s32 op(s16 weight) and sandbox_register() it.
 * (For training-style models, make op() write back via pva_write() to exercise
 *  the dirty-eviction path.)
 * ------------------------------------------------------------------------- */

static s32 op_square(s16 w){ s32 x = w; return x * x; }     /* "energy"   */
static s32 op_abs(s16 w){ return (w < 0) ? -w : w; }        /* "L1 norm"  */

struct model_reg { const char *name; s32 (*op)(s16); };
static struct model_reg regs[8];
static int nregs = 0;

static void sandbox_register(const char *name, s32 (*op)(s16)){
    if (nregs < 8){ regs[nregs].name = name; regs[nregs].op = op; nregs++; }
}

/* Fill the host-resident model with deterministic pseudo-random int16 weights. */
static void fill_model(void){
    u32 seed = 12345;
    for (u32 pi = 0; pi < MODEL_PAGES; pi++){
        u16 *p = (u16 *)pva_model_page(pi);
        for (u32 e = 0; e < ELEMS_PER_PAGE; e++){
            seed = seed * 1103515245u + 12345u;
            u32 v = seed & 0xFFFF;
            if (v > 32767) v -= 65536;
            p[e] = (s16)v;
        }
    }
}

/* Ground-truth computed directly on host backing (no paging). */
static s64 cpu_compute(s32 (*op)(s16)){
    s64 acc = 0;
    for (u32 pi = 0; pi < MODEL_PAGES; pi++){
        u16 *p = (u16 *)pva_model_page(pi);
        for (u32 e = 0; e < ELEMS_PER_PAGE; e++)
            acc += (s64)op((s16)p[e]);
    }
    return acc;
}

void sandbox_run(void){
    pva_init();
    fill_model();

    kprintf("[sandbox] VRAM=%u KB  model=%u KB  over-sub=%ux\n",
            DEV_PAGES * 4, MODEL_PAGES * 4, MODEL_PAGES / DEV_PAGES);

    sandbox_register("sum-of-squares", op_square);
    sandbox_register("abs-sum",       op_abs);

    for (int i = 0; i < nregs; i++){
        s64 ref = cpu_compute(regs[i].op);
        s64 got = pva_run_inference(regs[i].op);
        int ok  = (ref == got);
        kprintf("[sandbox] model='%s' ref=%lld got=%lld %s\n",
                regs[i].name, ref, got, ok ? "MATCH" : "MISMATCH");
    }

    kprintf("[sandbox] page-faults=%u  evictions=%u\n",
            pva_get_faults(), pva_get_evicts());
}
