# Red Daft OS — Architecture Overview

Red Daft OS is a coder-/security-first Linux distribution. This document describes
the *buildable* architecture. Sections marked **[DEFENSIVE REFRAME]** replace
spec items that, as originally written, describe rootkit/malware primitives or an
AI-safety jailbreak. Those are implemented instead as legitimate, authorization-gated
security-research capabilities.

## 1. Philosophy & Audience
- Target: red-teamers (authorized engagements only), security engineers, AI devs,
  coder-first power users.
- **Source-available, protectively curated.** Verbatim redistribution (commercial and
  non-commercial) is permitted; creation or distribution of modified versions /
  derivative OS forks requires prior written consent (see `LICENSE`). Sleek, fast,
  usable out-of-the-box, but the authors remain the sole curators of the OS lineage.
- Legal/ethical baseline: all offensive capability is **transparent, labeled, and
  requires explicit authorization scope**. Nothing hides itself by default.

## 2. System & Kernel  **[DEFENSIVE REFRAME]**
Original spec asked for stealth/process-hiding/DKOM/covert-channels as default.
Replaced with:
- **Daft-Kernel**: a *hardened* kernel (KSPP-aligned) plus an LKM framework for
  **authorized monitoring and attack detection** (the same primitives, used to
  *find* rootkits, not to *be* one). Version **7.1.10** (kernel.org) with
  `build/configs/kernel-config.x86_64` — KSPP hardening + full GPU enablement
  (amdgpu/radeon/nouveau, `CONFIG_DRM`, `ZONE_DEVICE`, `HMM`, `IOMMU`) +
  live-boot essentials.
- **Red Daft AI-Kernel v0.2 "Demo Box"**: from-scratch bare-metal x86_64 kernel
  (`kernel/ai-kernel/`) — Multiboot2 → long mode, 1 GiB hugepage identity, buddy PMM,
  heap, spinlocks, IDT (16-byte gates), PIT 100 Hz, PS/2, serial (COM1), PCI scan.
  Boots via GRUB `multiboot2` alongside Daft-Kernel; installed systems get
  `/boot/ai-kernel.elf` + `/etc/grub.d/40-ai-kernel`.
- **Dual-boot GRUB**: one menu, two kernels. `build/build-iso.sh:stage_iso` emits
  `grub.cfg` with `load_env` / `saved_entry` / `next_entry` support and a 1024-byte
  `grubenv` (via `grub-editenv`). `GRUB_DEFAULT=saved` is set in the rootfs so
  `daft-kernel` can make the choice persistent.
- **Kernel Switch CLI** (pairs across kernels):
  - *Demo Box*: `kernel status|list|switch <aik|linux>|reboot [name]|pool [0..9]|hmm` (`kctl` alias), `reboot`, `pool`, `uname` — RAM-only hint + `reboot()` via 8042 / CF9 / triple-fault (`kernel/ai-kernel/src/reboot.c:1`, `shell.c:1`).
  - *Linux*: `daft-kernel` (`packages/daft-kernel/daft-kernel.sh:1`) — `status|list|set|next|reboot` wrapping `grub-reboot` / `grub-set-default` / `grub-editenv`, normalizes `aik|linux|linux-safe` to GRUB titles.
- **Red-NetStack**: legitimate raw-socket + packet-crafting userspace tooling
  (scapy/libpcap-aligned) and a lab-grade tunneling/covert-channel *research*
  module that is OFF by default and logged.
- **Crypto-FS**: memory-backed tmpfs + LUKS-backed volatile volumes with
  fast secure-wipe (already a legit Linux feature set).

## 3. Memory Management — 10-Pool Tiered Design

Red Daft OS exposes **10 pools in both kernels** — the bare-metal HMM and the Linux VRAM Engine — with matching concepts but separate implementations.

### 3.1 HMM v3 (AI-Kernel, bare-metal) — `kernel/ai-kernel/src/hmm.{h,c}:1`

10 specialized pools sharing one elastic budget donated run-by-run from the buddy PMM:

| Pool | Name | Policy | Quota (max=512) | Role |
|------|------|--------|-----------------|------|
| 0 | weights | LRU_FREQ + prefetch | 30% 612 KiB | Model weights, streaming |
| 1 | kv | ARENA (never auto-evict) | 20% 408 KiB | KV-cache sessions |
| 2 | scratch | FIFO ring | 10% 204 KiB | Workspace scratch |
| 3 | activ | LRU | ~30p 120 KiB | Activations |
| 4 | embed | LRU | ~30p 120 KiB | Embeddings |
| 5 | attn | LRU | ~30p 120 KiB | Attention buffers |
| 6 | worksp | FIFO | ~29p 116 KiB | Extra workspace |
| 7 | cache | LRU | ~29p 116 KiB | Generic cache |
| 8 | tensor | LRU | ~29p 116 KiB | Tensor staging |
| 9 | generic | LRU | ~29p 116 KiB | Fallback / user |

* `hmm.h:23` defines `POOL_COUNT=10`, `pool:4` bits, `enum` with 10 entries.
* `hmm.c:86` `pool_grow()` carves `order`-rounded runs via `pmm_donate_range()`.
* `evict_weights()` (second-chance), `evict_fifo()` (scratch/worksp), `evict_lru()` (others), `ARENA` returns OOM.
* `load_new()` handles `host_base==0` hostless models, `host_addr` ternary, integrity `dbgmark('X')`.
* `hmm_init(128,512)` weighted quotas, initial donation to weights, `kprintf` of all 10 quotas.
* `hmm_stats()` prints 10 pools with `evict/hit/wb`. Demo in `kernel.c:5` streams 4 models (7/7 MATCH incl. t-verify), KV sessions (64 pages, 102-page quota), scratch ring (200 `scratch_next`), plus 10-pool exercise (1-page fault in each of pools 3..9).

Shell introspection: `kernel hmm` / `kernel pool [id]` / `pool` / `stats` / `mem`.

### 3.2 VRAM Engine (Linux, C++20 + CUDA/HIP) — `vram-engine/`

High-performance Custom VRAM Management Engine for 3B/7B/32B LLMs. Mirrors the 10-pool model but for Linux GPU memory:

| Pool | Name | Policy | Quota (2 GiB) | Role |
|------|------|--------|---------------|------|
| 0 | ModelWeights | LRU_FREQ | 35% 716 MiB | Read-only static weights |
| 1 | KvCache | ARENA | 25% 512 MiB | Dynamic paged KV |
| 2 | ActivationsTensors | FIFO | 15% 307 MiB | High-freq cyclic |
| 3 | WorkspaceScratchpad | FIFO | 5% 102 MiB | Temp kernels |
| 4 | HostSwapStaging | LRU_GENERIC | 5% 102 MiB | Pinned staging |
| 5 | EmbeddingBuffers | LRU_FREQ | 5% 102 MiB | Embeds + prefetch |
| 6 | QuantizationMetadata | LRU_GENERIC | 3% 61 MiB | Scales/codebooks |
| 7 | AsyncStreamQueue | LRU_GENERIC | 2% 40 MiB | Stream control blocks |
| 8 | SystemIpcShared | LRU_GENERIC | 3% 61 MiB | IPC handles |
| 9 | EmergencyOverflow | LRU_GENERIC | 2% 40 MiB | Lends to 1/2 |

* **Tiered:** VRAM (`cudaMalloc`/`hipMalloc`) for active tensors + DDR pinned (`cudaMallocHost`/`hipHostMalloc`, 4 dedicated `cudaStream_t`/`hipStream_t`, round-robin) for cold weights/long KV. `prefetch_to_vram()` / `offload_to_ddr()` are `cudaMemcpyAsync`/`hipMemcpyAsync` with optional `shadow_ptr` double-buffering (hide PCIe latency).
* **HAL:** `vram-engine/include/red_daft_vram.h:1` macro-switchable: `-DRD_USE_CUDA` → CUDA, `-DRD_USE_ROCM` → HIP, no flag → `malloc` CPU fallback (CI/Kaggle CPU). All alloc/free/memcpy/stream via `hal_*` wrappers.
* **Dynamic Borrowing & Anti-Starvation:** `VramEngine::borrow_memory()` — if Pool 1/2 pressure `>85%`, lend from Pool 9; if Emergency also pressured, first `offload_lru_to_host()` its cold blocks, then retry. Hysteresis `60%` on return. `KvCache` never auto-evicts (OOM reported).
* **API:** `red_daft_vram.h:1` defines `MemoryBlock`, `TieredBuffer`, `MemoryPool` (per-pool `mutex`, LRU list, `touch`/`lru_victim`), `VramEngine` (singleton, `shared_mutex`, `handle_to_pool_`, `next_block_id_`). Required deliverables: `initialize()`, `allocate(PoolType,size,align,tag)→BlockHandle`, `deallocate()`, `prefetch_to_vram()`, `offload_to_ddr()`, `borrow_memory()`, `print_pool_stats()`.
* **Python:** `vram-engine/src/pybind_wrapper.cpp:1` `PYBIND11_MODULE(red_daft_vram)` exposes `PoolType`, `PoolStats`, `EngineConfig`, `Block` (RAII), `allocate/allocate_handle/deallocate/prefetch/offload/borrow`, `allocate_torch()` (optional `torch/extension.h`), `stress_3b_benchmark(layers=28, hidden=3072)` (28-layer double-buffered streaming + KV paged + activation FIFO + emergency borrows) and `stress_all_pools()`. Build via `CMakeLists.txt:1` or `setup.py:1` (`pip install -e .`).
* **Tests:** `vram-engine/tests/test_vram.cpp:1` (10×1 MiB alloc, offload/prefetch round-trip, borrow, double-buffer), `benchmarks/stress_3b.py:1` (Kaggle one-liner).

### 3.3 LoRa Brain Engine (`vram-engine/src/red_daft_lora_manager.{h,cpp}`)

Modular LoRa adapter hot-swap engine for multi-LoRa serving on a shared base model (1B–3B). Manages microsecond-level swapping of LoRa adapters (10–50 MB each) between System DDR4/DDR5 (pinned host memory) and GPU VRAM across the 10 OS Memory Pools.

| LoRa Pool | VRAM Pool | Name | Role |
|-----------|-----------|------|------|
| 1 | 0–4 region | SystemControl | OS Control & System Agent LoRa |
| 2 | 0–4 region | ReasoningLogic | Reasoning & Logic LoRa |
| 3 | 0–4 region | CodingSyntax | Coding & Syntax LoRa |
| 4 | 0–4 region | ConversationLang | Conversation & Language LoRa |

* **Zero-copy / async hot-swapping:** Inactive LoRa weights live in Host Pinned Memory (`cudaMallocHost` / `hipHostMalloc`). Async loader uses `cudaMemcpyAsync` / `hipMemcpyAsync` on per-adapter CUDA/HIP Streams to pre-fetch the required adapter *before* the compute layer executes, avoiding GPU stalls. CPU fallback uses `std::memcpy` for CI/Kaggle.
* **SGMV-style weight patching:** `apply_lora_weights()` computes `output = base_output + B @ A @ input` on-the-fly without modifying the static base model weights in Pool 0. CPU fallback implements the full matrix multiply; GPU path provides device pointers for fused kernel dispatch.
* **LRU eviction:** `evict_lru_lora()` automatically evicts the Least Recently Used adapter back to System DDR RAM when VRAM is constrained, retaining only the active adapter(s) in Pools 1–4. `evict_all_except_active()` clears all cached adapters.
* **Thread-safety:** `LoRaRegistry` uses `std::shared_mutex` for registry state with per-adapter `std::mutex` for state transitions. `LoRaSwapper` uses `std::shared_mutex` for swap orchestration.
* **API:** `red_daft_lora_manager.h` defines `LoRaAdapter` (state machine: Unregistered→InDDR→Loading→Active→Evicting→InDDR), `LoRaRegistry` (register/lookup/LRU/touch), `LoRaSwapper` (hot_swap_async, swap_to_pool, swap_to_coding_lora, apply_lora_weights, evict_lru_lora, print_stats).
* **Python:** `vram-engine/src/lora_pybind_wrapper.cpp` `PYBIND11_MODULE(red_daft_lora)` exposes `LoRaPool`, `LoRaState`, `LoRaInfo`, `LoRaStats`, `initialize/register_lora/swap_to/swap_to_coding_lora/swap_to_reasoning_lora/evict_lru/print_stats/lora_stats`.
* **Tests:** `vram-engine/tests/test_lora.cpp` (9 tests: register 4 adapters, hot-swap sequence, LRU victim, SGMV patching, empty adapter, host data integrity, stats, evict-all, raw registration).
 * **Build:** `CMakeLists.txt` adds `red_daft_lora_manager.cpp` to `red_daft_vram` library and `lora_test` binary. `setup.py` adds LoRa sources to Python extension.

### 3.4 Nano-Context Engine (`vram-engine/src/red_daft_nano_context.{h,cpp}`)

Keeps **Base Model Weights strictly FP16 in VRAM Pool 0** and routes **all** Input/Output Context Window + KV Cache allocations to dynamically spawned, high-density **Nano-Pools**. Nano-Pools spawn on request start (`request_begin()`) and recycle on generation completion (`request_end()`), with a warm `free_list` (default 8) for low-latency re-spawn.

- **Separation of concerns:** `NanoPool` has no reference to `VramEngine`/Pool 0 — weights are never quantized, moved, or touched by this engine.
- **KV compression:** FP16 → INT4/INT2 per-channel quantization (scale + zero-point) packed into the Nano-Pool buffer ⇒ up to 4–8× density vs raw FP16, with an exact-dequant round-trip error of 0 on aligned ramps.
- **Ephemeral Token Stream Ring:** fixed-capacity ring (default `stream_capacity` < 50 MB per active stream) for continuous input/output token traffic; old tokens dropped on overflow (streaming semantics).
- **Lifecycle recycling:** `active_` map + `free_list_` vector under a `std::shared_mutex`; each `NanoPool` owns a per-pool `std::mutex`.
- **Dual-HAL device backing (AMD + NVIDIA):** when compiled with `-DRD_USE_CUDA` / `-DRD_USE_ROCM`, each `NanoPool` allocates a real device buffer (`cudaMalloc`/`hipMalloc`) for the packed KV cells + token ring, creates a per-pool async stream, and mirrors `kv_store`/`kv_load`/`stream_push` as `cudaMemcpyAsync`/`hipMemcpyAsync` host⇄device transfers. CPU fallback (`no flag`) uses `malloc`+`memcpy` and leaves the device mirror disabled, keeping the < 50 MB / stream SLA for CI/Kaggle.
- **API:** `NanoContextEngine::initialize/request_begin/request_end/kv_store/kv_load/stream_push/stream_pop/pool_stats/print_stats` + free-function convenience wrappers `nano_initialize/nano_request_begin/nano_request_end`.
- **Python:** `vram-engine/src/nano_pybind_wrapper.cpp` `PYBIND11_MODULE(red_daft_nano_context)` exposes `NanoContextConfig`, `NanoQuantStats`, `NanoStreamStats`, `NanoPoolStats`, `nano_initialize/request_begin/request_end/kv_store/kv_load/stream_push/stream_pop/active_pools/free_pools/pool_stats/print_stats`.
- **Build:** `CMakeLists.txt` adds `red_daft_nano_context.cpp` to `red_daft_vram` + `nano_test` binary + `red_daft_nano_context_py` pybind module. `setup.py` adds a third extension (one `PYBIND11_MODULE` per `.so`).

### 3.5 Red Daft Evaluation Engine (`red-eval`) — `vram-engine/src/red_eval_engine.{h,cpp}`

**Isolated, low-memory (< ~100 MB) async probe diagnostic engine.** Profiles the hot-swapped LLM/LoRa stack across the **5 Core Vectors** without mutating active VRAM Pool 0–9 states, emitting a structured JSON diagnostic report and exporting failure instances for LoRa fine-tuning.

| # | Skill vector            | Probe types                              | LoRa pool (Brain Engine) |
|---|-------------------------|------------------------------------------|--------------------------|
| a | GeneralLanguage         | Perplexity, MultipleChoice               | Pool 4 — ConversationLang|
| b | ReasoningMath           | Perplexity, MultipleChoice               | Pool 2 — ReasoningLogic  |
| c | CodeGenSyntax           | CodeGen (syntax validity)                | Pool 3 — CodingSyntax    |
| d | StructuredOutput        | JsonSchema (JSON / function-call)        | Pool 1 — SystemControl   |
| e | DomainKnowledge         | Domain (factoid verify)                  | Pool 1 — SystemControl (cross-cutting) |

- **Zero-copy IPC / LoRa hot-swap:** `RedCtlIpc` speaks a tiny Unix Domain Socket protocol to the AI-Kernel's `redctl` (`REDCTL` framing, op 1 = swap LoRa). When no live `redctl` exists (CPU / Kaggle / Colab), it routes in-process to `LoRaSwapper` for microsecond skill↔pool hot-swapping — toggles load/unload only the matching pool LoRa, never disturbing otherwise-active adapters.
- **Scoring matrix:** per-vector `overall`/`accuracy`/`latency_us`, cross-normalized from prompt-type verifiers (balanced JSON braces + expected-key presence, code brace/paren/bracket balance, exact MC match, perplexity fluency clamp).
- **Failure exporter:** hallucinated / syntax-broken / schema-breaking outputs are captured with prompt+expected+error and appended to `red_failed_dataset.jsonl` (per-probe id, skill, timestamp) for targeted LoRa fine-tuning.
- **Interfaces:** `include/red_eval_engine.h` (engine + `RedCtlIpc`), `src/red_eval_engine.cpp` (Diagnostic Logic + Kernel IPC), `src/red_eval_cli.cpp` (UI Rendering — `red-eval [--json|--tune|--export]` TUI), `src/eval_pybind_wrapper.cpp` (`PYBIND11_MODULE(red_eval)`), `red_eval_bridge.py` (Kaggle/Colab PyTorch wrapper + CPU emulator fallback), `tests/test_eval.cpp` (12 native assertions).
- **Build:** `CMakeLists.txt` adds `red_eval_engine.cpp` to `red_daft_vram` + `eval_test` + `red_eval_cli` binary + `red_eval_py` pybind module (4th module; one `PYBIND11_MODULE` per `.so`). `setup.py` adds a fourth extension. `build.yml` runs native `eval_test` + CLI `--json | jq` + Python bridge smoke.

## 4. GPU Subsystem

- **Kconfig fragment** `build/configs/kernel-config.x86_64` — `amdgpu`, `radeon`, `nouveau`, `TTM`, `ZONE_DEVICE`, `HMM`, `IOMMU`, `VFIO` etc., 35-pt audit.
- **Drivers baked in** even without HW (never probe); software fallbacks `llvmpipe` (GL), `lavapipe` (Vulkan), `PoCL` (OpenCL) keep apps running.
- **Tooling** `packages/gpu/` — `daft-gpu` (`status`/`activate amd|nvidia|fallback`) + `daft-compat` 100-pt scorer (`packages/gpu/check-compat.sh:1`: Kconfig 35 + firmware 10 + fallbacks 25 + ROCm 15 + CUDA 15). CI `gpu-compat.yml` enforces: real Kconfig audit, headless `nvcc` sm_86/89/120, `hipcc` gfx90a/gfx1100, bookworm fallback 25/25.
- **Stacks** `packages/specs/{nvidia,amd}/` — `.daftspec` for `amdgpu-dkms`, `rocm-runtime`, `cuda-toolkit` 12.9 (Blackwell `sm_120`), `cudnn`, `tensorrt`. `vram-engine` HAL reuses the same CUDA/ROCm headers.

## 5. Packaging & Compatibility
- `daft-pkg`: native `.daft` archive manager (tar.zst + manifest + signature).
- `daft-kernel`: GRUB switcher (see §2).
- `deb-adapter`: resolves and installs `.deb` via `apt`/`dpkg` with `.so`
  dependency satisfaction from Ubuntu repos.

## 6. AI & Dev Ecosystem
- VSCodium + extensions, PyTorch, CUDA/ROCm configured.
- Ollama + `qwen2.5:3b` default; CLI toggles for 7B/14B/32B by resource.
- `daft-ai-guard`: *defensive* content gate (blocks weaponized/illegal payloads).
- `vram-engine` (see §3.2) is the primary LLM memory path on Linux; `daft-ai-guard` v2 sits in front of Ollama (`127.0.0.1:11435→11434`) with ALLOW/BOOST/BLOCK + scope `none|lab|engagement`.
- System prompt: authorized-scope security research assistant (no guardrail bypass).

## 7. UX
- Daft-Shell: terminal-first, in-RAM C/Python execution via TCC/Cranelift.
- First-run "Classified ID Card" TUI (cosmetic, randomized cyber identifiers).
- Plymouth `reddaft` theme, `arc` + `papirus`, `daft-motd`, `lightdm` autologin `agent`.

## 8. Build & ISO Pipeline

`build/build-iso.sh:1` stages:
1. `stage_kernel_build` — kernel.org 7.1.10 from source (`build-kernel.sh`), `daft-defmon.ko` vs built `Module.symvers`.
2. `stage_ai_kernel` — `make -C kernel/ai-kernel` → `ai-kernel.elf` (QEMU smoke `make smoke` via GRUB rescue ISO, `timeout 240`, 7/7 MATCH).
3. `stage_bootstrap` — `mmdebstrap` bookworm + `PKGS` (systemd, XFCE, mesa, rocm/cuda deps, live-boot etc.).
4. `stage_kernel_install` — `vmlinuz` + modules + `ai-kernel.elf` + `grub.d/40-ai-kernel` + `config`/`System.map`/`Module.symvers` + `depmod`.
5. `stage_configure` — copy `packages`/`ai`/`shell`/`ux`/`kernel`/`vram-engine` to `/opt/daft`, create `agent` (sudo NOPASSWD, autologin `getty@tty1`), `os-release` → `Red Daft OS 0.2`, XFCE configs, `daft-firstrun.service`, `daft-ai-guard.service`, `daft-gpu.service`, `daft-pkg`/`daft-kernel` symlinks, `GRUB_DEFAULT=saved` for `daft-kernel`.
6. `gen_assets` — `gen-assets.py` wallpaper.
7. `stage_iso` — `mksquashfs` → `filesystem.squashfs`, `grub.cfg` with `load_env`/`saved_entry`/`next_entry` + 3 menuentries (live, safe, aik) + `grubenv` (1024 B via `grub-editenv`), `grub-mkrescuehybrid` BIOS+EFI ISO `iso/red-daft-os-*.iso` (~430 MiB).

`KEEP_WORK=1` keeps `build/work` for incremental builds; otherwise trap cleans up.

## 9. Directory Layout
```
build/          ISO pipeline, hardened+GPU kernel config, Plymouth, assets gen
packages/       daft-pkg, deb-adapter, daft specs, gpu/ (daft-gpu + scorer), daft-kernel/
ai/             Modelfile, ollama-setup, daft-ai-guard v2 (+proxy, service)
shell/          daft-shell (in-RAM execution)
kernel/         daft-defmon LKM + ai-kernel/ (bare-metal Demo Box, 10 pools: HMM v3)
vram-engine/    C++20 tiered manager: 10 pools, VRAM+DDR pinned, CUDA/HIP HAL, pybind11
                  LoRa Brain Engine: async hot-swap, LRU eviction, SGMV patching
                  Nano-Context Engine: INT4/INT2 KV, token stream ring, Nano-Pools
                  Red-burst Ultra-LoRA Engine: 3s GPU spike, 3 streams (KV quantize /
                    LoRA re-project / page align + pool alloc), watchdog,
                    INT4/FP4/INT2 packing, tensor-core HAL (red_daft_gpu.h)
                  Isolated Evaluation Engine (red-eval): 5 skill vectors, JSON report,
                    --tune LoRa hot-swap TUI, red_failed_dataset.jsonl exporter
                  include/red_daft_vram.h  include/red_daft_lora_manager.h
                  include/red_daft_nano_context.h  include/red_eval_engine.h
                  include/red_daft_gpu.h  include/red_burst_engine.h
                  src/red_daft_vram.cpp  src/red_daft_lora_manager.cpp
                  src/red_daft_nano_context.cpp  src/red_eval_engine.cpp
                  src/red_burst_engine.cpp
                  src/pybind_wrapper.cpp  src/lora_pybind_wrapper.cpp
                  src/nano_pybind_wrapper.cpp  src/eval_pybind_wrapper.cpp
                  src/red_eval_cli.cpp  src/red_burst_cli.cpp
                  red_eval_bridge.py
                  tests/test_vram.cpp  tests/test_lora.cpp  tests/test_nano.cpp
                  tests/test_eval.cpp  tests/test_burst.cpp
                  benchmarks/stress_3b.py  CMakeLists.txt  setup.py
 ux/             branding, MOTD, first-run ID card, installer
docs/           architecture (this file)
.github/        ci workflows (iso, build, gpu-compat, ai-kernel, rocm, cuda)
```

## 10. CI / Verification

| Workflow | What it proves |
|---|---|
| `iso.yml` | Full distro builds; hybrid BIOS+EFI ISO artifact |
| `build.yml` | Container image, `daft-pkg` install, `ai-guard` v2, `vram-engine` CPU smoke (`vram_test`, `stress_all_pools`), `lora_test` (LoRa Brain Engine: 9 tests), `nano_test` (Nano-Context Engine: KV INT4/INT2, token stream, pool recycle), `eval_test` + `red-eval --json | jq` (Evaluation Engine: 12 assertions, 5-vector JSON report) + `red_daft_nano_context` Python smoke and `red_eval_bridge` PyTorch-bridge smoke |
| `gpu-compat.yml` | Kconfig audit ≥33/35, nvcc sm_86/89/120, hipcc gfx90a/gfx1100, fallback 25/25 |
| `ai-kernel.yml` | Demo Box **10 pools** — QEMU boot, 7/7 MATCH (incl. t-verify WRITEBACK), KV/scratch/10-pool, elasticity 128 pages |
| `amd-rocm.yml` / `nvidia-cuda.yml` | Live-repo package validation + toolchain smoke |

Native pre-push: `clang --target=x86_64 -fsyntax-only` for `shell.c`/`hmm.c`/`kernel.c`, host harness `h10` (mmap 0x2000000, `hmm_init(128,512)`, 7×MATCH + 10×pool OK), `g++` build `vram_test` (10 pools OK).
