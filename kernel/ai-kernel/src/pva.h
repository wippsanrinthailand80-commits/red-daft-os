#ifndef PVA_H
#define PVA_H
#include "kernel.h"

/* AI subsystem parameters.
 * DEV_PAGES  = simulated "VRAM" (scarce, fast device memory)
 * MODEL_PAGES = full model footprint (lives in host RAM, paged on demand)
 * We deliberately over-subscribe MODEL_PAGES >> DEV_PAGES to prove the
 * Heterogeneous Memory Manager keeps results correct under constrained VRAM. */
#define DEV_PAGES       256          /* 1 MB "VRAM"  */
#define MODEL_PAGES     1024         /* 4 MB "model" */
#define ELEMS_PER_PAGE  (PAGE_SIZE / 2)   /* int16 weights per 4KB page */

void pva_init(void);
u8  *pva_model_page(u32 pi);
s64  pva_run_inference(s32 (*op)(s16));
u32  pva_get_faults(void);
u32  pva_get_evicts(void);

#endif
