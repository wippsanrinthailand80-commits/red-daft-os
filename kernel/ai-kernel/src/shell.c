/* shell.c — the Demo Box: interactive serial console. "Do anything." */
#include "kernel.h"
#include "hmm.h"

int  model_verify(u32 m);
const char *model_name(u32 m);
u32  model_count(void);

static char line[128]; static u32 len;

/* ---- kernel-switch state (bare-metal: RAM-only hint; persistent via daft-kernel on Linux side) ---- */
static char next_target[16] = {0};   /* "linux" | "aik" | "" (=keep) */

static void prompt(void){ kprintf("\ndaftbox> "); }

/* tiny helpers */
static int starts_with(const char *s, const char *pfx){
    while(*pfx) if(*s++ != *pfx++) return 0;
    return 1;
}
static const char *skip_spaces(const char *p){ while(*p==' ') p++; return p; }
static int streql(const char *a,const char *b){ return kstrcmp(a,b)==0; }

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
            "  kernel status    show running kernel + HMM pools\n"
            "  kernel list      list bootable kernels (GRUB menu)\n"
            "  kernel switch <name>  set next-boot target: aik|linux (hint; use daft-kernel on Linux for persistence)\n"
            "  kernel reboot [name]  set target and reboot now\n"
            "  reboot [name]    alias for kernel reboot\n"
            "  uname            alias for kernel status\n"
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

/* ---- kernel subcommands ---- */
static void kernel_status(void){
    kprintf("kernel: Red Daft AI-Kernel v0.2 'Demo Box' (HMM v3)\n");
    kprintf("arch:   x86_64 bare-metal | multiboot2 | buddy PMM | PIT 100Hz\n");
    kprintf("uptime: %llu ticks (~%llus)\n", ticks(), ticks()/100);
    kprintf("next:   %s\n", next_target[0] ? next_target : "(default)");
    hmm_stats();
    kprintf("[pmm]  free=%lluKB total=%lluKB\n", pmm_free_pages()*4, pmm_total()/1024);
}
static void kernel_list(void){
    kprintf("boot entries (GRUB):\n");
    kprintf("  * aik    Red Daft AI-Kernel v0.2 — experimental bare-metal HMM  [running]\n");
    kprintf("    linux  Red Daft OS 0.2 — Daft-Kernel 7.1.10 (Live)            [reboot to switch]\n");
    kprintf("    linux-safe  Red Daft OS 0.2 — safe graphics (nomodeset)\n");
    if(next_target[0]) kprintf("next boot hint: %s (select in GRUB or use daft-kernel on Linux)\n", next_target);
    else kprintf("hint: 'kernel switch linux' + 'reboot' to try the Linux kernel; 'daft-kernel set <name>' on Linux makes it persistent.\n");
}
static void kernel_switch(const char *arg){
    arg = skip_spaces(arg);
    if(!*arg){ kprintf("usage: kernel switch <aik|linux|linux-safe>\n"); kernel_list(); return; }
    /* normalize */
    const char *norm=NULL;
    if(streql(arg,"aik")||streql(arg,"ai-kernel")||streql(arg,"demo")) norm="aik";
    else if(streql(arg,"linux")||streql(arg,"daft")||streql(arg,"daft-kernel")) norm="linux";
    else if(streql(arg,"linux-safe")||streql(arg,"safe")) norm="linux-safe";
    else { kprintf("unknown target '%s' (try: aik, linux, linux-safe)\n", arg); return; }
    /* store RAM-only hint */
    usize n=kstrlen(norm); if(n>=sizeof(next_target)) n=sizeof(next_target)-1;
    kmemset(next_target,0,sizeof(next_target)); kmemcpy(next_target,norm,n);
    kprintf("[kernel] next boot target set to '%s' (RAM hint)\n", next_target);
    kprintf("  bare-metal: reboot and pick '%s' in the GRUB menu (timeout 10s).\n", next_target);
    kprintf("  installed:  run 'sudo daft-kernel set %s' inside Linux to make it sticky via grub-reboot/set-default.\n", next_target);
}
static void kernel_reboot(const char *arg){
    arg = skip_spaces(arg);
    if(*arg){
        /* allow 'kernel reboot linux' as shorthand for switch+reboot */
        /* only accept known targets */
        if(streql(arg,"aik")||streql(arg,"ai-kernel")||streql(arg,"demo")||
           streql(arg,"linux")||streql(arg,"daft")||streql(arg,"daft-kernel")||
           streql(arg,"linux-safe")||streql(arg,"safe")){
            kernel_switch(arg);
        } else {
            kprintf("unknown target '%s'\n", arg); return;
        }
    } else if(next_target[0]){
        kprintf("[kernel] rebooting to '%s' (hint)...\n", next_target);
    } else {
        kprintf("[kernel] rebooting...\n");
    }
    /* small grace for serial to flush */
    for(volatile int i=0;i<200000;i++) io_wait();
    if(next_target[0]) reboot_to(next_target);
    else reboot();
}
static void do_kernel(const char *arg){
    arg = skip_spaces(arg);
    if(!*arg || streql(arg,"status") || streql(arg,"info") || streql(arg,"uname")){
        kernel_status(); return;
    }
    if(streql(arg,"list") || streql(arg,"ls")){
        kernel_list(); return;
    }
    if(starts_with(arg,"switch")){
        const char *rest = arg+6;
        kernel_switch(rest); return;
    }
    if(starts_with(arg,"reboot")){
        const char *rest = arg+6;
        kernel_reboot(rest); return;
    }
    if(streql(arg,"hmm") || streql(arg,"stats")){
        hmm_stats(); return;
    }
    if(streql(arg,"help")){
        kprintf("kernel subcommands:\n"
                "  kernel status          show running kernel + HMM + uptime\n"
                "  kernel list            list GRUB boot entries\n"
                "  kernel switch <name>   set next-boot hint (aik|linux|linux-safe)\n"
                "  kernel reboot [name]   switch (optional) and reboot now\n"
                "  kernel hmm             alias for HMM stats\n");
        return;
    }
    kprintf("unknown kernel subcommand '%s' (try: kernel help)\n", arg);
}

static void exec(char *l){
    /* tokenize: cmd + rest */
    char *sp=l; while(*sp==' ') sp++;
    char *rest=sp;
    while(*rest && *rest!=' ') rest++;
    if(*rest){ *rest=0; rest++; }
    if(!*sp) return;
    rest = (char*)skip_spaces(rest);
    if(!kstrcmp(sp,"help")) help();
    else if(!kstrcmp(sp,"about"))
        kprintf("Red Daft AI-Kernel v0.2 'Demo Box'\nbare-metal x86_64 | buddy PMM | HMM v3 elastic VRAM cache\n");
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
    /* kernel-switch system */
    else if(!kstrcmp(sp,"kernel") || !kstrcmp(sp,"kctl")) do_kernel(rest);
    else if(!kstrcmp(sp,"reboot")) kernel_reboot(rest);
    else if(!kstrcmp(sp,"uname")) kernel_status();
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
