/* shell.c — the Demo Box: interactive serial console. "Do anything." */
#include "kernel.h"
#include "hmm.h"

int  model_verify(u32 m);
const char *model_name(u32 m);
u32  model_count(void);

static char line[128]; static u32 len;

static void prompt(void){ kprintf("\ndaftbox> "); }

static void help(void){
    kprintf("commands:\n"
            "  ls               list models\n"
            "  verify <m|all>   stream through HMM, check vs CPU reference\n"
            "  stats            HMM + PMM statistics\n"
            "  restore <pages>  give VRAM back to main RAM allocator\n"
            "  pci              enumerate PCI devices\n"
            "  mem              memory map summary\n"
            "  uptime           ticks since boot\n"
            "  demo             full showcase (verify all + stats)\n"
            "  clear|about|help\n");
}

static void do_verify(const char *arg){
    if(!kstrcmp(arg,"all")){
        int ok=1;
        for(u32 m=0;m<model_count();m++) ok &= model_verify(m);
        hmm_stats();
        kprintf("[demo] %s\n", ok?"ALL MODELS MATCH — pager is correct":"FAILURES PRESENT");
    } else {
        int m = arg[0]-'0';
        if(m<0 || (u32)m>=model_count()){ kprintf("bad model id\n"); return; }
        model_verify(m); hmm_stats();
    }
}

static void exec(char *l){
    /* tokenize: cmd + rest */
    char *sp=l; while(*sp==' ') sp++;
    char *rest=sp;
    while(*rest && *rest!=' ') rest++;
    if(*rest){ *rest=0; rest++; }
    if(!*sp) return;
    if(!kstrcmp(sp,"help")) help();
    else if(!kstrcmp(sp,"about"))
        kprintf("Red Daft AI-Kernel v0.2 'Demo Box'\nbare-metal x86_64 | buddy PMM | HMM v2 elastic VRAM cache\n");
    else if(!kstrcmp(sp,"clear")) kprintf("\n\n\n");
    else if(!kstrcmp(sp,"ls")){
        for(u32 m=0;m<model_count();m++) kprintf("  [%u] %s\n",m,model_name(m));
    }
    else if(!kstrcmp(sp,"verify")) do_verify(*rest?rest:"all");
    else if(!kstrcmp(sp,"stats")){ hmm_stats();
        kprintf("[pmm] free=%lluKB total=%lluKB\n", pmm_free_pages()*4, pmm_total()/1024); }
    else if(!kstrcmp(sp,"restore")){
        u32 n=0; for(char *p=rest;*p>='0'&&*p<='9';p++) n=n*10+(*p-'0');
        if(!n){ kprintf("usage: restore <pages>\n"); return; }
        u64 f=hmm_restore_to_pmm(n);
        kprintf("[restore] gave %llu pages back to RAM\n",f); hmm_stats();
    }
    else if(!kstrcmp(sp,"pci")) pci_scan();
    else if(!kstrcmp(sp,"mem"))
        kprintf("pool=%lluKB ram_restored=%lluKB amplification=%llu/100x\n",
                hmm_pool_pages()*4, 0ULL, hmm_amplification_x100());
    else if(!kstrcmp(sp,"uptime")) kprintf("%llu ticks (~%llus)\n",ticks(),ticks()/100);
    else if(!kstrcmp(sp,"demo")){
        kprintf("--- Red Daft Demo Box ---\n");
        do_verify("all");
        kprintf("VRAM amplification right now: %llu/100x effective\n",
                hmm_amplification_x100());
        u64 f=hmm_restore_to_pmm(64);
        kprintf("elastic pool: returned %llu pages to main RAM on demand.\n",f);
    }
    else kprintf("unknown: '%s' (try help)\n", sp);
}

void shell_run(void){
    kprintf("\n=== Red Daft Demo Box === type 'help'\n");
    prompt();
    for(;;){
        int c;
        while((c=serial_pollc())>=0){
            char ch=(char)c;
            if(ch=='\r'||ch=='\n'){
                con_putc('\n'); line[len]=0;
                if(len){ exec(line); len=0; }
                prompt();
            } else if(ch==8||ch==127){
                if(len){ len--; con_puts("\b \b"); }
            } else if(ch>=32 && ch<127 && len<sizeof(line)-1){
                line[len++]=ch; con_putc(ch);
            }
        }
        char k=kbd_pollkey();
        if(k){
            if(k=='\n'){ con_putc('\n'); line[len]=0;
                if(len){ exec(line); len=0; }
                prompt();
            } else if(len<sizeof(line)-1){ line[len++]=k; con_putc(k); }
        }
        __asm__ volatile("hlt");
    }
}
