/* kernel.c — boot sequence + auto-demo, then hands over to the Demo Box. */
#include "kernel.h"
#include "hmm.h"

void kernel_main(u64 mbi){
    (void)mbi;
    serial_init();
    con_init();
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  Red Daft AI-KERNEL v0.2  --  Demo Box\n");
    kprintf("  from-scratch x86_64 | buddy PMM | HMM v2\n");
    kprintf("=============================================\n");

    pmm_init(NULL,0);
    heap_init();
    idt_init();
    pit_init(100);                 /* 100 Hz tick */
    pci_scan();

    /* elastic VRAM: start at 512KB, cap 2MB — models total 16MB, so the
     * pager must sustain >=8x amplification while staying correct. */
    if(hmm_init(128, 512)!=0) panic("hmm init failed");
    models_init();

    kprintf("[boot] running self-demo...\n");
    int ok=1;
    for(u32 m=0;m<model_count();m++) ok &= model_verify(m);
    hmm_stats();
    u64 f = hmm_restore_to_pmm(128);
    kprintf("[boot] elasticity check: returned %llu pages to RAM\n", f);
    kprintf("[boot] %s - entering demo box\n", ok?"SELF-TEST PASS":"SELF-TEST FAIL");
    shell_run();
}
