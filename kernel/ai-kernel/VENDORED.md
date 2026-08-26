# kernel/ai-kernel — Red Daft AI-Kernel v0.2 "Demo Box"

v0.1 was vendored from the standalone AI-Kernel experiment (see git history
and the original upload tree). v0.2 is a fresh implementation built to spec:

- x86_64 from scratch: Multiboot2 -> long mode -> identity-mapped 4GB,
  buddy PMM, kernel heap, spinlocks, IDT/PIT/PS2-keyboard, PCI scan.
- **HMM v3 (10 pools)** — the point of the exercise: 10 elastic pools
  (`weights/kv/scratch/activ/embed/attn/worksp/cache/tensor/generic`) sharing
  one budget donated from the buddy PMM, weighted quotas 30/20/10/40, per-pool
  policies (LRU_FREQ weights/embed, FIFO scratch/worksp, ARENA kv, LRU generic),
  run-by-run donation, dirty writeback + per-migration integrity — streaming
  16MB of models through ≤2MB with 7/7 MATCH, KV/scratch/10-pool exercises.
- **Demo Box**: interactive serial shell (`help`, `verify`, `stats`,
  `restore`, `pci`, `demo`, `kernel status|list|switch|reboot|pool|hmm`,
  `reboot`, `pool`, ...). Boots into a full self-test + demo; `kernel` CLI
  pairs with Linux `daft-kernel` for GRUB `saved_entry`/`next_entry` switching.

## Build & smoke
```bash
make -C kernel/ai-kernel            # build/ai-kernel.elf
make -C kernel/ai-kernel smoke      # QEMU headless, gates serial output
```
CI: `.github/workflows/ai-kernel.yml` runs both on every push. The ISO
pipeline installs the ELF as a second GRUB entry ("Red Daft AI-Kernel").

