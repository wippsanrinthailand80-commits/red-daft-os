# kernel/ai-kernel — Red Daft AI-Kernel v0.2 "Demo Box"

v0.1 was vendored from the standalone AI-Kernel experiment (see git history
and the original upload tree). v0.2 is a fresh implementation built to spec:

- x86_64 from scratch: Multiboot2 -> long mode -> identity-mapped 4GB,
  buddy PMM, kernel heap, spinlocks, IDT/PIT/PS2-keyboard, PCI scan.
- **HMM v2** (the point of the exercise): an elastic VRAM pool that is
  *donated from / restored to* the physical allocator, LRU eviction with
  frequency tie-break, sequential prefetch, dirty writeback — streaming
  16MB of models through <=2MB of pool with automatic CPU-reference
  verification on every run.
- **Demo Box**: interactive serial shell (`help`, `verify`, `stats`,
  `restore`, `pci`, `demo`, ...). Boots into a full self-test + demo.

## Build & smoke
```bash
make -C kernel/ai-kernel            # build/ai-kernel.elf
make -C kernel/ai-kernel smoke      # QEMU headless, gates serial output
```
CI: `.github/workflows/ai-kernel.yml` runs both on every push. The ISO
pipeline installs the ELF as a second GRUB entry ("Red Daft AI-Kernel").

