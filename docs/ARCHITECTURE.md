# Red Daft OS — Architecture Overview

Red Daft OS is a coder-/security-first Linux distribution. This document describes
the *buildable* architecture. Sections marked **[DEFENSIVE REFRAME]** replace
spec items that, as originally written, describe rootkit/malware primitives or an
AI-safety jailbreak. Those are implemented instead as legitimate, authorization-gated
security-research capabilities.

## 1. Philosophy & Audience
- Target: red-teamers (authorized engagements only), security engineers, AI devs,
  coder-first power users.
- 100% FOSS. Sleek, fast, usable out-of-the-box.
- Legal/ethical baseline: all offensive capability is **transparent, labeled, and
  requires explicit authorization scope**. Nothing hides itself by default.

## 2. System & Kernel  **[DEFENSIVE REFRAME]**
Original spec asked for stealth/process-hiding/DKOM/covert-channels as default.
Replaced with:
- **Daft-Kernel**: a *hardened* kernel (KSPP-aligned) plus an LKM framework for
  **authorized monitoring and attack detection** (the same primitives, used to
  *find* rootkits, not to *be* one).
- **Red-NetStack**: legitimate raw-socket + packet-crafting userspace tooling
  (scapy/libpcap-aligned) and a lab-grade tunneling/covert-channel *research*
  module that is OFF by default and logged.
- **Crypto-FS**: memory-backed tmpfs + LUKS-backed volatile volumes with
  fast secure-wipe (already a legit Linux feature set).

## 3. Packaging & Compatibility
- `daft-pkg`: native `.daft` archive manager (tar.zst + manifest + signature).
- `deb-adapter`: resolves and installs `.deb` via `apt`/`dpkg` with `.so`
  dependency satisfaction from Ubuntu repos.

## 4. AI & Dev Ecosystem
- VSCodium + extensions, PyTorch, CUDA/ROCm configured.
- Ollama + `qwen2.5:3b` default; CLI toggles for 7B/14B/32B by resource.
- `daft-ai-guard`: *defensive* content gate (blocks weaponized/illegal payloads).
- System prompt: authorized-scope security research assistant (no guardrail bypass).

## 5. UX
- Daft-Shell: terminal-first, in-RAM C/Python execution via TCC/Cranelift.
- First-run "Classified ID Card" TUI (cosmetic, randomized cyber identifiers).
