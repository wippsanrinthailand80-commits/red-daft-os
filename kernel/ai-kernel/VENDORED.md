# kernel/ai-kernel — vendored from the AI-Kernel project

Upstream: standalone "AI-Kernel (QEMU / from-scratch x86_64)" repository
(single-commit tree, no public remote at vendor time). Vendored into Red Daft OS
to become the second bootable kernel of the distribution.

## What it is
A from-scratch Multiboot2 x86_64 kernel demonstrating the Heterogeneous Memory
Manager: a paravirtual accelerator with a small VRAM pool, GPU-style page
faults, LRU eviction and host<->device migration — with an automatic
correctness gate (every compute result is verified against a CPU reference).

## Build
```bash
make -C kernel/ai-kernel        # -> build/ai-kernel.elf (needs x86_64 gcc)
make -C kernel/ai-kernel run    # boots under QEMU, serial on stdio
```
The ISO pipeline builds it automatically and installs it as:
- `/boot/ai-kernel.elf` in the live system (GRUB entry: "Red Daft AI-Kernel"),
- `/opt/daft/kernel/ai-kernel/` sources for hacking inside the OS.

## Relationship to Red Daft OS
This kernel is the proving ground for the `accel_hal` seam and the Daft
Interchange IR (see docs/): the same HMM logic runs natively here, and ports to
the hardened Linux 7.1.10 kernel via CONFIG_HMM_MIRROR/ZONE_DEVICE (enabled in
build/configs/kernel-config.x86_64).
