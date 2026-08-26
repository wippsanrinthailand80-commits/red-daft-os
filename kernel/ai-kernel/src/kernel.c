/* kernel.c — boot sequence + auto-demo, then hands over to the Demo Box. */
#include "kernel.h"
#include "hmm.h"

void kernel_main(u64 mbi){
    (void)mbi;
    serial_init();
    con_init();
    dbgmark('1');
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  Red Daft AI-KERNEL v0.2  --  Demo Box\n");
    kprintf("  from-scratch x86_64 | buddy PMM | HMM v2\n");
    kprintf("=============================================\n");

    dbgmark('2');
    pmm_init(NULL,0);
    heap_init();
    dbgmark('3');
    idt_init();
    dbgmark('p');
    pit_init(100);                 /* 100 Hz tick */
    dbgmark('s');
    __asm__ volatile("sti");
    dbgmark('n');                  /* survived bare sti (no unmasked IRQs) */
    outb(0x21,0xFE);               /* now unmask ONLY the timer */
    sleep_ticks(3);                /* let it fire a few times */
    dbgmark('4');
    pci_scan();

    dbgmark('5');
    /* elastic VRAM: start at 512KB, cap 2MB — models total 16MB, so the
     * pager must sustain >=8x amplification while staying correct. */
    if(hmm_init(128, 512)!=0) panic("hmm init failed");
    dbgmark('6');
    models_init();

    dbgmark('7');
    kprintf("[boot] running self-demo...\n");
    extern u32 hb0_sum(void);
    u32 sum_before = hb0_sum();
    int ok=1;
    for(u32 m=0;m<model_count();m++){
        dbgmark('A'+(char)m);
        ok &= model_verify(m);
        u32 sum_now = hb0_sum();
        if(sum_now != sum_before){
            kprintf("[!] host_buf[0] CORRUPTED after model %u (sum %u -> %u)\n",
                    m, sum_before, sum_now);
            sum_before = sum_now;
        }
    }
    dbgmark('8');
    hmm_stats();
    u64 f = hmm_restore_to_pmm(128);
    kprintf("[boot] elasticity check: returned %llu pages to RAM\n", f);
    dbgmark('9');
    kprintf("[boot] %s - entering demo box\n", ok?"SELF-TEST PASS":"SELF-TEST FAIL");
    shell_run();
}
