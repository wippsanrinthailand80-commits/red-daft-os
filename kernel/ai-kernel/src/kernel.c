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
    kprintf("  from-scratch x86_64 | buddy PMM | HMM v3\n");
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
    /* inference: stream all models through the weights pool */
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
    /* training: gradient writes must survive eviction via dirty writeback */
    dbgmark('B');
    kprintf("[train] gradient pass on 'abs-sum'...\n");
    if(train_model(1)<0){ ok=0; }
    (void)model_verify(2);              /* eviction pressure */
    (void)model_verify(3);
    if(model_verify_trained(1)){
        kprintf("[train] WRITEBACK OK - dirty pages survived eviction\n");
    } else { kprintf("[train] WRITEBACK FAIL\n"); ok=0; }

    /* kv-cache sessions: arena semantics, explicit OOM, clean recycle */
    dbgmark('C');
    {
        int s1=kv_session_begin();
        u64 va=s1>=0?kv_session_alloc(s1,64):0;
        int okv = va!=0;
        if(okv) ((u64*)(usize)va)[0]=0xDAF7DAF7ull;
        if(s1>=0) kv_session_end(s1);
        int s2=kv_session_begin();
        u64 vb=s2>=0?kv_session_alloc(s2,64):0;
        if(s2>=0) kv_session_end(s2);
        if(okv&&vb) kprintf("[kv] OK session alloc/free/realloc\n");
        else { kprintf("[kv] FAIL (va=%llx vb=%llx)\n",(unsigned long long)va,(unsigned long long)vb); ok=0; }
    }
    /* scratch ring: rotation under pressure */
    dbgmark('D');
    {
        int rot=0,anyrot=0;
        for(int i=0;i<200;i++){ scratch_next(1024,&rot); anyrot|=rot; }
        if(anyrot) kprintf("[scratch] OK ring rotated under pressure\n");
        else { kprintf("[scratch] FAIL (no rotation)\n"); ok=0; }
    }
    dbgmark('8');
    hmm_stats();
    u64 f = hmm_restore_to_pmm(128);
    kprintf("[boot] elasticity check: returned %llu pages to RAM\n", f);
    dbgmark('9');
    kprintf("[boot] %s - entering demo box\n", ok?"SELF-TEST PASS":"SELF-TEST FAIL");
    shell_run();
}
