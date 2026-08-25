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
