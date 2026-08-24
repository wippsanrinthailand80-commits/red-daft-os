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
- **Branding** — crimson `#FF0033` / matte-black theme, ASCII logo, MOTD, Plymouth splash.

## Build
```bash
sudo bash build/build-iso.sh      # bootstrap -> configure -> kernel -> plymouth -> ISO
```
Requires `mmdebstrap`, `squashfs-tools`, `xorriso`, and kernel headers in the rootfs.

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

## Directory layout
```
build/        ISO pipeline + hardened kernel config + Plymouth theme
packages/     daft-pkg, deb-adapter, daft specs (incl. amd/ ROCm specs)
ai/           Modelfile, ollama-setup, daft-ai-guard
shell/        daft-shell (in-RAM execution)
kernel/       daft-defmon defensive LKM
ux/           branding, MOTD, first-run ID card
docs/         architecture
```

## License
GPL-3.0-or-later. See `LICENSE`.
