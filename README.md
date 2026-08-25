# Red Daft OS

A coder-/security-first Linux distribution for red-teamers, security engineers,
AI developers, and power users. 100% free and open-source.

> **Responsible use.** Red Daft OS ships offensive-security *tooling*, not
> stealth malware. Every capability is transparent, labeled, and authorization-
> gated. The kernel is **hardened** (KSPP-aligned) and includes a *defensive*
> monitor (`daft-defmon`) that detects rootkit/DKOM behavior. The local AI guard
> (`daft-ai-guard`) blocks weaponized/illegal content. Use only in authorized
> scopes (your own systems, labs, written engagements).

## Highlights
- **Daft-Kernel** — hardened kernel config (`build/configs/kernel-config.x86_64`).
- **daft-pkg** — native `.daft` package manager (signed tar.zst + manifest).
- **deb-adapter** — install Ubuntu `.deb` with `.so` deps resolved.
- **Local AI** — Ollama + `qwen2.5:3b` default; `ollama-setup scale 7b|14b|32b`.
- **daft-ai-guard** — defensive content filter (no guardrail bypass).
- **daft-shell** — in-RAM C/Python execution (TCC).
- **AMD ROCm support** — `.daft` specs for the full ROCm stack (see below).
- **NVIDIA CUDA support** — `.daft` specs for CUDA drivers, toolkit, cuDNN, TensorRT.
- **GPU-ready everywhere** — drivers compiled in even without hardware, software fallbacks, `daft-gpu`/`daft-compat` tooling (see below).
- **Branding** — crimson `#FF0033` / matte-black theme, ASCII logo, MOTD, Plymouth splash.

## System requirements

| | Minimum | Recommended | AI / ML |
|---|---|---|---|
| **RAM** | 2 GB | 8 GB | 16 GB+ |
| **Storage** | 20 GB | 50 GB | 100 GB+ |
| **CPU** | x86_64, 2 cores | x86_64, 4 cores+ | x86_64, 8 cores+ |

- **Minimum** runs XFCE desktop + daft-shell + security tooling.
- **Recommended** adds Ollama + `qwen2.5:3b` local AI model.
- **AI / ML** accommodates larger models (7B–32B) and GPU-accelerated workloads (AMD ROCm / NVIDIA CUDA).
- GPU drivers are optional: `amdgpu-dkms` for AMD, `nvidia-driver-550+` for NVIDIA.

## Build
```bash
sudo bash build/build-iso.sh      # bootstrap -> configure -> kernel -> plymouth -> ISO
```
Requires `mmdebstrap`, `squashfs-tools`, `xorriso`, `grub-efi-<arch>-bin`, and root privileges (`CAP_SYS_ADMIN`).

The ISO is also built automatically on every push by `.github/workflows/iso.yml`
(privileged GitHub runner) and published as a downloadable **red-daft-os-iso**
artifact. It boots a real live system: GRUB → casper → hardened kernel, with the
`agent` user auto-logged into an **XFCE desktop** (or `daft-shell` on tty1), plus
a **disk installer** (`reddaft-install`) to install it to a drive.

## Container (runs anywhere, no boot required)
```bash
docker build -t red-daft-os .
docker run -it --rm red-daft-os          # boots into the ID card + daft-shell
```
CI builds this image on every push (`.github/workflows/build.yml` → `container-image`).

## Package manager
```bash
daft-pkg install ./foo.daft       # install a local .daft archive
daft-deb add <pkg.deb>            # install an Ubuntu .deb (deps resolved)
make-daft-pkg.sh <pkgdir> <name> <version> [out.daft]   # build a .daft
```

## GPU compatibility (with or without hardware)

Red Daft OS is built to be **GPU-ready on every machine**: drivers are compiled
in even where no card exists (they simply never probe), and a software
fallback (llvmpipe GL, lavapipe Vulkan, PoCL OpenCL) keeps every app running
on GPU-less systems. Measure it any time:

```bash
daft-gpu status          # what was detected + which mode is active
daft-compat              # capability score out of 100 (CI-enforced >= threshold)
```

### Supported cards

| Tier | Cards | Path |
|---|---|---|
| **NVIDIA Blackwell** | RTX 50xx (`sm_120`) | nvidia-open 570+, CUDA ≥ 12.8 (default track: 12.9) |
| **NVIDIA Ada / Ampere** | RTX 4090 (`sm_89`), RTX 3090 (`sm_86`) | nvidia-open/GSP + CUDA |
| **AMD RDNA4** | RX 9000 series | amdgpu + DCN (recent kernel + linux-firmware) |
| **AMD RDNA3** | RX 7900 XTX (`gfx1100`) | amdgpu + ROCm compute |
| **AMD RDNA1/2** | RX 5000/6000 | amdgpu; Vulkan via Mesa RADV |
| **AMD GCN4-5** | RX 400/500, Vega, Radeon VII | amdgpu native; Mesa RADV/OpenCL |
| **AMD GCN1-2** | R9 270X/280X-class | amdgpu SI/CIK (experimental tier) |
| **Pre-GCN** | HD 2000-6000 | legacy `radeon` driver |

Compute-stack support narrows on old silicon: ROCm targets `gfx90a+`
(MI200/MI300) and `gfx1100+`; Polaris-and-older gets display + Vulkan +
Mesa OpenCL — not ROCm. That is upstream reality, not a Red Daft gap.

### How compatibility is validated without hardware
`.github/workflows/gpu-compat.yml` runs on every push:
1. resolves our hardened fragment against real kernel Kconfig (catches
   silently-dropped options) and enforces kernel readiness ≥ 33/35;
2. compiles CUDA for `sm_86`/`sm_89`/`sm_120` headlessly via nvcc;
3. compiles HIP for `gfx90a` (MI200) and `gfx1100` (RX 7900 XTX);
4. bootstraps a bookworm rootfs with the fallback set and enforces
   FALLBACK 25/25 in the scorer.

## Dual kernel

The ISO ships **two kernels** in one GRUB menu:

1. **Daft-Kernel** — hardened Linux 7.1.10, the daily driver (XFCE, security
   tooling, local AI, GPU stacks).
2. **Red Daft AI-Kernel** — a from-scratch bare-metal Multiboot2 kernel
   (`kernel/ai-kernel/`, serial console on COM1) demonstrating the
   Heterogeneous Memory Manager: VRAM-as-cache over host RAM with GPU-style
   page faults, LRU eviction, and automatic CPU-reference correctness gates.

Both boot from the same medium; installed systems get `/boot/ai-kernel.elf`
plus a `grub.d` entry automatically. The long-term goal is a shared
`accel_hal` seam and a vendor-neutral interchange format so compute kernels
written once run on either kernel, on NVIDIA or AMD hardware.

## AMD / ROCm packages
Specs under `packages/specs/amd/` build `.daft` archives for AMD GPUs:
`amdgpu-dkms`, `rocm-runtime`, `rocblas`, `miopen`, `rocm-smi`, `rocm-dev`,
`pytorch-rocm`. Validate against the live AMD repo without downloading:
```bash
bash packages/specs/amd/rocm-runtime.daftspec check
```
Full stage + build:
```bash
ROCM_VER=6.2 bash packages/specs/amd/rocm-runtime.daftspec
```

## NVIDIA / CUDA packages
Specs under `packages/specs/nvidia/` build `.daft` archives for NVIDIA GPUs:
`cuda-drivers`, `cuda-toolkit`, `cudnn`, `tensorrt`. Validate against the live CUDA repo:
```bash
bash packages/specs/nvidia/cuda-toolkit.daftspec check
```
Full stage + build:
```bash
CUDA_VER=12.6 bash packages/specs/nvidia/cuda-toolkit.daftspec
```

## Directory layout
```
build/        ISO pipeline + hardened kernel config + Plymouth theme
packages/     daft-pkg, deb-adapter, daft specs (amd/ ROCm + nvidia/ CUDA)
ai/           Modelfile, ollama-setup, daft-ai-guard
shell/        daft-shell (in-RAM execution)
kernel/       daft-defmon defensive LKM
ux/           branding, MOTD, first-run ID card
docs/         architecture
```

## License
GPL-3.0-or-later. See `LICENSE`.
