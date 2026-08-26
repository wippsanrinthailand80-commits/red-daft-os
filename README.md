# Red Daft OS

A coder-/security-first Linux distribution for red-teamers, security engineers,
AI developers, and power users. 100% free and open-source.

> **Responsible use.** Red Daft OS ships offensive-security *tooling*, not
> stealth malware. Every capability is transparent, labeled, and authorization-
> gated. The kernel is **hardened** (KSPP-aligned) and includes a *defensive*
> monitor (`daft-defmon`) that detects rootkit/DKOM behavior. The local AI
> guard (`daft-ai-guard` v2) blocks weaponized content while actively
> supporting authorized security work. Use only in authorized scopes (your own
> systems, labs, written engagements).

## Highlights
- **Dual kernel** — hardened Linux 7.1.10 daily driver + **Red Daft AI-Kernel**
  v0.2 "Demo Box", a from-scratch bare-metal x86_64 kernel (see below).
- **Daft-Kernel** — hardened config (`build/configs/kernel-config.x86_64`):
  KSPP-aligned hardening + full GPU enablement + live-boot essentials.
- **daft-pkg** — native `.daft` package manager (signed tar.zst + manifest).
- **deb-adapter** — install Ubuntu `.deb` with `.so` deps resolved.
- **Local AI** — Ollama + `qwen2.5-coder:3b` default; `ollama-setup scale 7b|14b|32b`.
- **daft-ai-guard v2** — context-aware filter: ALLOW / BOOST / BLOCK routing
  with engagement-scope gating and a local audit log. Not a bypass — the
  opposite: it makes the filter smarter, not weaker.
- **daft-shell** — in-RAM C/Python execution (TCC).
- **GPU-ready everywhere** — drivers compiled in even without hardware,
  software fallbacks (llvmpipe/lavapipe/PoCL), `daft-gpu`/`daft-compat`
  tooling with a 100-point capability score (see below).
- **AMD ROCm support** — `.daft` specs for the full ROCm stack.
- **NVIDIA CUDA support** — CUDA 12.9 track (Blackwell-ready), cuDNN, TensorRT.
- **Branding** — crimson `#FF3366` / matte-black theme, ASCII logo, MOTD,
  Plymouth splash.

## System requirements

| | Minimum | Recommended | AI / ML |
|---|---|---|---|
| **RAM** | 2 GB | 8 GB | 16 GB+ |
| **Storage** | 20 GB | 50 GB | 100 GB+ |
| **CPU** | x86_64, 2 cores | x86_64, 4 cores+ | x86_64, 8 cores+ |

- **Minimum** runs XFCE desktop + daft-shell + security tooling.
- **Recommended** adds Ollama + `qwen2.5-coder:3b` local model.
- **AI / ML** accommodates larger models (7B–32B via `ollama-setup scale`) and
  GPU-accelerated workloads (AMD ROCm / NVIDIA CUDA).
- GPU drivers optional at install: `amdgpu-dkms` (AMD), nvidia-open 570+
  (NVIDIA). The kernel already contains everything both need.

## Build
```bash
sudo bash build/build-iso.sh          # full pipeline -> iso/*.iso
KEEP_WORK=1 sudo -E bash build/build-iso.sh   # incremental (reuses built kernel)
```
Requires `mmdebstrap`, `squashfs-tools`, `xorriso`, `grub-efi-amd64-bin`,
`grub-pc-bin`, `mtools`, kernel build deps, and root. Failed builds keep their
workdir for post-mortem; successful ones clean up automatically.

The ISO is also built automatically on every push by `.github/workflows/iso.yml`
and published as a downloadable **red-daft-os-iso** artifact (~430 MB): GRUB →
live-boot → hardened kernel, `agent` auto-logged into **XFCE**, plus a disk
installer (`reddaft-install`).

## Container (runs anywhere, no boot required)
```bash
docker build -t red-daft-os .
docker run -it --rm red-daft-os          # ID card + daft-shell
```

## Package manager
```bash
daft-pkg install ./foo.daft       # install a local .daft archive
daft-deb add <pkg.deb>            # install an Ubuntu .deb (deps resolved)
make-daft-pkg.sh <pkgdir> <name> <version> [out.daft]
```

## GPU compatibility (with or without hardware)

Drivers are compiled into the kernel even where no card exists (they simply
never probe); a software fallback (llvmpipe GL, lavapipe Vulkan, PoCL OpenCL)
keeps every app running on GPU-less machines.

```bash
daft-gpu status          # detected GPUs + active mode
daft-gpu activate amd|nvidia|fallback
daft-compat              # capability score out of 100 (CI-enforced)
```

### Supported cards

| Tier | Cards | Path |
|---|---|---|
| **NVIDIA Blackwell** | RTX 50xx (`sm_120`) | nvidia-open 570+, **CUDA 12.9** default |
| **NVIDIA Ada / Ampere** | RTX 4090 (`sm_89`), RTX 3090 (`sm_86`) | nvidia-open/GSP + CUDA |
| **AMD RDNA4** | RX 9000 series | amdgpu + DCN |
| **AMD RDNA3** | RX 7900 XTX (`gfx1100`) | amdgpu + ROCm compute |
| **AMD RDNA1/2** | RX 5000/6000 | amdgpu; Vulkan via Mesa RADV |
| **AMD GCN4-5** | RX 400/500, Vega, Radeon VII | amdgpu native; RADV/OpenCL |
| **AMD GCN1-2** | R9 270X/280X-class | amdgpu SI/CIK tier |
| **Pre-GCN** | HD 2000-6000 | legacy `radeon` driver |

Compute stacks narrow on old silicon: ROCm targets `gfx90a+`/`gfx1100+`;
older cards get display + Vulkan + Mesa OpenCL. Upstream reality, not a gap.

Kernel-side readiness (35 pts), firmware (10), fallbacks (25), ROCm (15) and
CUDA (15) are scored by `packages/gpu/check-compat.sh`; CI enforces thresholds
on every push via `.github/workflows/gpu-compat.yml`:
- real-Kconfig audit of our fragment (catches silently-dropped options),
- headless `nvcc` compiles for sm_86/sm_89/sm_120,
- headless `hipcc` compiles for gfx90a/gfx1100,
- bookworm rootfs bootstrap proving FALLBACK 25/25.

## Dual kernel

One GRUB menu, two kernels:

**1. Daft-Kernel** — hardened Linux 7.1.10; the daily driver (XFCE, security
tooling, local AI, GPU stacks).

**2. Red Daft AI-Kernel v0.2 "Demo Box"** — a from-scratch bare-metal x86_64
kernel (`kernel/ai-kernel/`) that proves the Heterogeneous Memory Manager:

- Multiboot2 → long mode → identity paging; buddy PMM; kernel heap;
  spinlocks; PIT/PS2/serial; PCI enumeration.
- **HMM v2**: an elastic VRAM pool that is *donated from* and *restored to*
  the physical allocator on demand; LRU eviction with frequency tie-break,
  sequential prefetch, dirty writeback; per-migration integrity checks.
 - Streams models through ≤2 MB of pool with automatic CPU-reference
   verification of every result (CI-proven: 1024 faults, 512 evictions, 4/4 MATCH).
 - Interactive serial **Demo Box** shell: `help ls verify stats restore pci mem uptime demo`
   plus **`kernel` switcher** — `kernel status|list|switch|reboot`, `reboot`, `uname`
   (pairs with Linux-side `daft-kernel`).

```bash
make -C kernel/ai-kernel smoke   # builds ELF + boots it under QEMU headlessly
# inside Demo Box:
kernel list                # => aik (running), linux, linux-safe
kernel switch linux        # hint for next boot
kernel reboot linux        # switch + reboot now
reboot                     # just reboot
# inside Linux (installed or live):
daft-kernel status
sudo daft-kernel set aik
sudo daft-kernel next linux && sudo reboot
```

Both kernels boot from the same medium; installed systems get `/boot/ai-kernel.elf`
plus a `grub.d` entry automatically. `daft-kernel` makes the choice persistent via
`grub-reboot` / `grub-set-default` (needs `GRUB_DEFAULT=saved`). The ISO's GRUB
now honors `saved_entry`/`next_entry` from `grubenv`. Roadmap: shared `accel_hal`
seam and a vendor-neutral interchange IR so compute kernels written once run on
either kernel, on NVIDIA or AMD hardware.

## Local AI + daft-ai-guard v2

Default stack: Ollama serving `qwen2.5-coder:3b` behind the guard proxy
(`127.0.0.1:11435` → `11434`). The proxy routes every prompt:

| Verdict | Meaning | Effect |
|---|---|---|
| **ALLOW** | benign or explicitly-scoped security work | forwarded untouched |
| **BOOST** | offensive ask without context | authorization-scoped system prompt injected so the model answers competently instead of refusing |
| **BLOCK** | weaponization ops (ransomware deployment, mass exploitation, undisclosed 0day stockpiles, targeting named parties) | HTTP 403 + audit |

Scope gate (offensive codegen requires ≥ lab):
```bash
python3 /opt/daft/ai/daft-ai-guard.py scope set lab     # none|lab|engagement
python3 /opt/daft/ai/daft-ai-guard.py selftest           # policy self-test
tail /var/lib/daft-ai-guard/audit.log                    # every decision
```

## AMD / ROCm packages
Specs under `packages/specs/amd/`: `amdgpu-dkms`, `rocm-runtime`, `rocblas`,
`miopen`, `rocm-smi`, `rocm-dev`, `pytorch-rocm`.
```bash
bash packages/specs/amd/rocm-runtime.daftspec check      # repo validation only
ROCM_VER=6.2 bash packages/specs/amd/rocm-runtime.daftspec
```

## NVIDIA / CUDA packages
Specs under `packages/specs/nvidia/`: `cuda-drivers`, `cuda-toolkit`,
`cudnn`, `tensorrt`. Default track **CUDA 12.9** (RTX 50xx-capable); override
with `CUDA_VER=12.6` etc.
```bash
bash packages/specs/nvidia/cuda-toolkit.daftspec check
CUDA_VER=12.9 bash packages/specs/nvidia/cuda-toolkit.daftspec
```

## CI

| Workflow | What it proves |
|---|---|
| `iso.yml` | full distro builds; hybrid BIOS+EFI ISO artifact |
| `build.yml` | container image, daft-pkg install, ai-guard v2 policy tests |
| `gpu-compat.yml` | kernel Kconfig audit ≥33/35, nvcc sm_86/89/120, hipcc gfx90a/gfx1100, fallback rootfs 25/25 |
| `ai-kernel.yml` | Demo Box builds + QEMU boot + 4/4 model verification + elasticity |
| `amd-rocm.yml` / `nvidia-cuda.yml` | live-repo package validation + toolchain smoke |

## Directory layout
```
build/          ISO pipeline, hardened+GPU kernel config, Plymouth, assets gen
packages/       daft-pkg, deb-adapter, daft specs, gpu/ (daft-gpu + scorer)
ai/             Modelfile, ollama-setup, daft-ai-guard v2 (+proxy, service)
shell/          daft-shell (in-RAM execution)
kernel/         daft-defmon LKM + ai-kernel/ (bare-metal Demo Box)
ux/             branding, MOTD, first-run ID card, installer
docs/           architecture
.github/        ci workflows (iso, build, gpu-compat, ai-kernel, rocm, cuda)
```

## License
GPL-3.0-or-later. See `LICENSE`.
