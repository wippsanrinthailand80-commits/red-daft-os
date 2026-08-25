#include "kernel.h"
#include "pva.h"

void kernel_main(void *mbi){
    serial_init();
    kprintf("AI-KERNEL: from-scratch x86_64, QEMU target\n");

    pmm_init(mbi);
    kprintf("AI-KERNEL: PMM free pages=%u (%u MB)\n",
            (u32)pmm_pages(), (u32)((pmm_pages() * PAGE_SIZE) / 1048576));

    sandbox_run();

    kprintf("AI-KERNEL: halted.\n");
    for (;;) asm("hlt");
}
