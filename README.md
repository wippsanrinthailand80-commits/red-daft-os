# Red Daft OS

A coder-/security-first Linux distribution for red-teamers, security engineers,
AI developers, and power users. **Source-available** — verbatim redistribution
(commercial and non-commercial) is permitted; creation or distribution of
modified versions / derivative OS forks requires prior written consent.

> **Responsible use.** Red Daft OS ships offensive-security *tooling*, not
> stealth malware. Every capability is transparent, labeled, and authorization-
> gated. The kernel is **hardened** (KSPP-aligned) and includes a *defensive*
> monitor (`daft-defmon`) that detects rootkit/DKOM behavior. The local AI
> guard (`daft-ai-guard` v2) blocks weaponized content while actively
> supporting authorized security work. Use only in authorized scopes (your own
> systems, labs, written engagements).

## Highlights

- **Dual kernel** — hardened Linux 7.1.10 daily driver + **Red Daft AI-Kernel**
  v0.2 "Demo Box", a from-scratch bare-metal x86_64 kernel.
- **HMM v3 — 10 pools** — AI-Kernel exposes 10 elastic VRAM pools with LRU/FIFO/arena
  policies, weighted quotas, and `kernel pool` CLI.
- **VRAM Engine** — Linux C++20 tiered manager (VRAM + DDR pinned) for 3B/7B/32B LLMs:
  10 pools, async `cudaMemcpyAsync`/`hipMemcpyAsync` double-buffering, HAL (CUDA/ROCm/CPU),
  emergency borrowing + anti-starvation, `pybind11` + PyTorch integration.
- **LoRa Brain Engine** — Dynamic LoRa adapter hot-swap: 4 LoRa pools (SystemControl,
  ReasoningLogic, CodingSyntax, ConversationLang), async host↔VRAM transfer, LRU eviction,
  SGMV-style weight patching (`B @ A @ input`), zero-copy on active adapters.
- **Nano-Context Engine** — Ephemeral high-density context manager: base weights stay
  strictly FP16 in Pool 0 while all Input/Output context + KV Cache route to spawned
  Nano-Pools with FP16→INT4/INT2 KV quantization and a < 50 MB token stream ring.
- **Red-burst Ultra-LoRA Engine** — 3-second compute spike (100% GPU utilization) to
  dynamically quantize active LoRA adapters and KV cache into INT4/FP4/INT2 micro-buffers
  routed into the 10-pool system. 3 parallel streams (KV quantize / LoRA re-project /
  page align + pool alloc), isolated watchdog thread enforces strict 3.00s cap + thermal
  throttling. Tensor-core (NVIDIA WMMA) / Matrix-core (AMD MFMA) abstraction via
  `red_daft_gpu.h`. CLI: `redctl lora load --mode ultra`.
- **Daft-Kernel** — hardened config (`build/configs/kernel-config.x86_64`):
  KSPP-aligned hardening + full GPU enablement + live-boot essentials.
- **daft-pkg** — native `.daft` package manager (signed tar.zst + manifest).
- **deb-adapter** — install Ubuntu `.deb` with `.so` deps resolved.
- **Local AI** — Ollama + `qwen2.5-coder:3b` default; `ollama-setup scale 7b|14b|32b`.
- **daft-ai-guard v2** — context-aware filter: ALLOW / BOOST / BLOCK routing
  with engagement-scope gating and a local audit log.
- **daft-shell** — in-RAM C/Python execution (TCC).
- **GPU-ready everywhere** — drivers compiled in even without hardware,
  software fallbacks (llvmpipe/lavapipe/PoCL), `daft-gpu`/`daft-compat`
  tooling with a 100-point capability score.
- **AMD ROCm support** — full ROCm stack: `amdgpu-dkms`, `rocm-runtime`, `rocblas`,
  `miopen`, `rocm-smi`, `rocm-dev`, `pytorch-rocm`.
- **NVIDIA CUDA support** — CUDA 12.9 track (Blackwell-ready), cuDNN, TensorRT.
- **Branding** — crimson `#FF3366` / matte-black theme, ASCII logo, MOTD,
  Plymouth splash.
- **CI** — 6 workflows on every push: ISO, build+tests, GPU compat, AI-Kernel smoke,
  AMD ROCm repo validation, NVIDIA CUDA repo validation.
- **Container** — Docker image with full stack at `/opt/daft/vram-engine`.

## System requirements

| Tier | RAM | Storage | CPU | What runs |
|---|---|---|---|---|
| **Minimum** | 2 GB | 20 GB | x86_64, 2 cores | XFCE desktop + daft-shell + security tooling + daft-pkg |
| **Recommended** | 8 GB | 50 GB | x86_64, 4 cores+ | + Ollama `qwen2.5-coder:3b` + VRAM Engine + LoRa Brain Engine |
| **AI / ML** | 16–32 GB | 100–200 GB | x86_64, 8 cores+ | + 7B–32B models (`ollama-setup scale`) + GPU (CUDA/ROCm) + 4 LoRa adapters |
| **Kaggle / CI** | 8 GB | 30 GB | Any | CPU fallback (no GPU needed — engine + benchmarks still run) |

### RAM breakdown (Recommended tier, 8 GB)

| Component | RAM | Notes |
|---|---|---|
| XFCE desktop + system | ~1.5 GB | systemd, LightDM, mesa |
| Ollama (`qwen2.5-coder:3b`) | ~2.5 GB | Inference working set |
| VRAM Engine overhead | ~0.1 GB | C++ singleton, 10 pool descriptors |
| LoRa Brain Engine | ~0.2 GB | 4 LoRa adapters (10–50 MB each) in DDR pinned |
| daft-ai-guard v2 proxy | ~0.05 GB | Python reverse proxy on `:11435` |
| daft-defmon LKM | ~0.01 GB | Defensive kernel monitor |
| daft-shell (TCC) | ~0.05 GB | In-RAM C/Python compiler |
| Buffer/cache | ~3.6 GB | Available for VMs, workloads, compile |

### Storage breakdown (Recommended tier, 50 GB)

| Component | Size | Notes |
|---|---|---|
| Base OS (XFCE + systemd) | ~8 GB | bookworm rootfs |
| Kernel + modules | ~0.5 GB | 7.1.10 hardened, GPU drivers baked in |
| AI-Kernel ELF + GRUB entry | ~0.002 GB | `/boot/ai-kernel.elf` |
| Ollama + models | ~5–20 GB | 3B (2 GB) up to 32B (19 GB) |
| Virtual Memory Engine (VRAM + LoRa + Nano-Context) | ~0.015 GB | C++20, ~5000 lines total |
| Security tooling | ~2 GB | nmap, metasploit-framework, scapy, etc. |
| daft-pkg + specs | ~0.5 GB | Package manager, GPU specs, kernel configs |
| Plymouth + branding | ~0.05 GB | Splash, wallpaper, MOTD, ID card assets |
| `/opt/daft/` full stack | ~0.02 GB | All engines + AI + shell + packages |

### Storage breakdown (AI/ML tier, 100–200 GB)

| Component | Size | Notes |
|---|---|---|
| All of Recommended | ~36 GB | See above |
| 7B–14B models | +10–30 GB | `ollama-setup scale 7b|14b` |
| 32B model | +19 GB | `ollama-setup scale 32b` |
| LoRa adapters (4×) | +0.2 GB | 10–50 MB each, hot-swappable |
| CUDA/ROCm toolkit | +5–15 GB | Driver + runtime + dev headers |
| VRAM Engine VRAM budget | (GPU) | 2 GiB default, auto-scales to 80% free VRAM |

## Build

### ISO (full OS)
```bash
sudo bash build/build-iso.sh                       # full pipeline → iso/*.iso
KEEP_WORK=1 sudo -E bash build/build-iso.sh        # incremental (reuses built kernel)
```
Requires `mmdebstrap`, `squashfs-tools`, `xorriso`, `grub-efi-amd64-bin`,
`grub-pc-bin`, `mtools`, kernel build deps, and root. Failed builds keep their
workdir for post-mortem; successful ones clean up automatically.

The ISO is also built automatically on every push by `.github/workflows/iso.yml`
and published as a downloadable **red-daft-os-iso** artifact (~430 MB): GRUB →
live-boot → hardened kernel, `agent` auto-logged into **XFCE**, plus a disk
installer (`reddaft-install`).

### AI-Kernel (bare-metal, 10 pools)
```bash
make -C kernel/ai-kernel              # → build/ai-kernel.elf (~2 MB)
make -C kernel/ai-kernel smoke        # QEMU headless: 7/7 MATCH, 10-pool, elasticity
```

### VRAM Engine (Linux, C++20, 10 pools + LoRa Brain Engine)
```bash
# CPU fallback (CI / Kaggle / no GPU)
cmake -B /tmp/vram-build -S vram-engine -DUSE_CUDA=OFF && cmake --build /tmp/vram-build -j
/tmp/vram-build/vram_test             # 10×1 MiB, offload/prefetch, borrow → PASS
/tmp/vram-build/lora_test             # 9 LoRa tests → ALL PASS

# NVIDIA CUDA
cmake -B /tmp/vram-build -S vram-engine -DUSE_CUDA=ON && cmake --build /tmp/vram-build -j

# AMD ROCm
cmake -B /tmp/vram-build -S vram-engine -DUSE_ROCM=ON && cmake --build /tmp/vram-build -j

# Python (pybind11 + optional torch)
pip install -e ./vram-engine              # CPU fallback
RD_USE_CUDA=1 pip install -e ./vram-engine
RD_USE_ROCM=1 pip install -e ./vram-engine

python vram-engine/benchmarks/stress_3b.py --iters 3    # 3B stress, 28 layers
python vram-engine/benchmarks/stress_3b.py --torch       # + PyTorch integration
```

### Container (runs anywhere, no boot required)
```bash
docker build -t red-daft-os .
docker run -it --rm red-daft-os          # ID card + daft-shell + VRAM + LoRa at /opt/daft/vram-engine
```

### Package manager
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
- **HMM v3 (10 pools)**: elastic VRAM pools *donated from* and *restored to*
  the physical allocator on demand. 10 pools
  (`weights/kv/scratch/activ/embed/attn/worksp/cache/tensor/generic`)
  with per-pool policies: LRU+freq+prefetch (weights), arena never-evict (KV),
  FIFO (scratch/worksp), generic LRU (others); dirty writeback + per-migration
  integrity checks. Weighted quotas (30%/20%/10%/40% over 7) with run-by-run donation.
- Streams models through ≤2 MB of pool with automatic CPU-reference
  verification of every result (CI-proven: ~2055 faults, ~1895 evictions, 7/7 MATCH
  incl. training writeback, plus KV/scratch/10-pool exercises).
- Interactive serial **Demo Box** shell: `help ls verify stats restore pci mem uptime demo`
  plus **`kernel` switcher** — `kernel status|list|switch|reboot|pool|hmm`,
  `reboot`, `pool`, `uname` (pairs with Linux-side `daft-kernel`). `kernel pool`
  lists 10 pools; `pool <id>` inspects.

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

## VRAM Engine — 10-Pool Tiered Manager (Linux, C++20 + CUDA/HIP)

High-performance **Custom VRAM Management Engine** for training/inference of 3B/7B/32B
LLMs while strictly minimizing VRAM. System DDR4/DDR5 is a high-speed secondary tier
(pinned `cudaMallocHost` / `hipHostMalloc`) with async prefetch/offload on dedicated
streams and double-buffering.

**Location:** `vram-engine/` — header `include/red_daft_vram.h`, impl `src/red_daft_vram.cpp`,
Python binding `src/pybind_wrapper.cpp` (`pybind11` + optional `torch/extension.h`).
**~5,000 lines** of C++20 across the VRAM, LoRa, and Nano-Context engines.

### 10 Memory Pools

| Pool | Name | Policy | Quota (2 GiB budget) | Role |
|---|---|---|---|---|
| 0 | ModelWeights | LRU_FREQ | 35% 716 MiB | Read-only static weights |
| 1 | KvCache | ARENA | 25% 512 MiB | Dynamic paged KV, never auto-evict |
| 2 | ActivationsTensors | FIFO | 15% 307 MiB | High-frequency cyclic |
| 3 | WorkspaceScratchpad | FIFO | 5% 102 MiB | Temp kernel workspace |
| 4 | HostSwapStaging | LRU_GENERIC | 5% 102 MiB | Pinned staging |
| 5 | EmbeddingBuffers | LRU_FREQ | 5% 102 MiB | Embeddings + prefetch |
| 6 | QuantizationMetadata | LRU_GENERIC | 3% 61 MiB | Scales / codebooks |
| 7 | AsyncStreamQueue | LRU_GENERIC | 2% 40 MiB | Stream control blocks |
| 8 | SystemIpcShared | LRU_GENERIC | 3% 61 MiB | IPC handles |
| 9 | EmergencyOverflow | LRU_GENERIC | 2% 40 MiB | Lends to Pool 1/2 under pressure |

All pools share one elastic budget donated run-by-run from the HAL; `hmm_pool_pages()` style
quotas are weighted but lendable. Pool 9 is the **Dynamic Borrowing Pool**.

### Tiered Memory (VRAM + DDR)

- **High-Speed Tier (VRAM):** active tensors, current layer, immediate KV — `cudaMalloc` / `hipMalloc`.
- **Capacity Tier (DDR pinned):** cold weights + long-context KV — `cudaMallocHost` / `hipHostMalloc`.
- **Async Pipeline:** `prefetch_to_vram()` / `offload_to_ddr()` are `cudaMemcpyAsync` / `hipMemcpyAsync`
  on 4 dedicated streams, with optional `shadow_ptr` double-buffering to hide PCIe latency.
  No GPU stall — next layer prefetches while current computes.

### HAL (Hardware Abstraction Layer)

Macro-switchable at compile time, single header:

```bash
-DRD_USE_CUDA   # NVIDIA  → cudaMalloc / cudaMemcpyAsync / cudaStream_t
-DRD_USE_ROCM   # AMD ROCm → hipMalloc / hipMemcpyAsync / hipStream_t
# no flag       # CPU fallback → malloc + memcpy simulation (CI/Kaggle CPU)
```

All device/host alloc, free, memcpy, streams go through `hal_*` wrappers. Auto-detection
in `setup.py` checks `/opt/rocm/` (AMD) then `/usr/local/cuda/` (NVIDIA) if no env var set.

### Dynamic Borrowing & Anti-Starvation

If Pool 1 (KV) or Pool 2 (Activations) hits `>85%` pressure, `borrow_memory()` requests
chunks from Pool 9. If Emergency also pressured, it first offloads its own LRU cold
blocks to host (`offload_lru_to_host`), then retries. Hysteresis at `60%` prevents
thrashing. KV is `ARENA` — OOM is reported, not silently dropped (vLLM semantics).

### Python / PyTorch Binding (Kaggle/Linux)

```bash
pip install pybind11 torch
pip install -e ./vram-engine              # CPU fallback
RD_USE_CUDA=1 pip install -e ./vram-engine
RD_USE_ROCM=1 pip install -e ./vram-engine

python -c "import red_daft_vram as rdv; help(rdv)"
```

```python
import red_daft_vram as rdv
rdv.initialize()  # auto 80% free VRAM or 2 GiB fallback
h = rdv.allocate_handle(0, 1<<20, tag="layer_0")  # Pool 0, 1 MiB
rdv.prefetch_to_vram(h, double_buffer=True)
rdv.offload_to_ddr(h)
rdv.deallocate(h)
rdv.print_pool_stats()

# Torch integration (optional)
t = rdv.allocate_torch(pool=0, shape=[3072,3072], dtype="bf16")

# 3B stress — streams 28 layers through 10 pools
res = rdv.stress_3b_benchmark(layers=28, hidden=3072, seq_len=2048, iterations=3, verbose=True)
print(res.report)  # peak VRAM, borrows, offloads, PASS/FAIL
```

See `vram-engine/README.md`, `CMakeLists.txt`, `benchmarks/stress_3b.py`, `tests/test_vram.cpp`.

## LoRa Brain Engine — Dynamic Adapter Hot-Swap (Linux, C++20 + CUDA/HIP)

Modular LoRa adapter hot-swap engine for multi-LoRa serving on a shared base model (1B–3B).
Manages microsecond-level swapping of LoRa adapters (10–50 MB each) between System DDR4/DDR5
(pinned host memory) and GPU VRAM across the 10 OS Memory Pools.

**Location:** `vram-engine/` — header `include/red_daft_lora_manager.h`, impl `src/red_daft_lora_manager.cpp`,
Python binding `src/lora_pybind_wrapper.cpp`. **~2,000 lines** of C++20.

### 4 LoRa Pools

| LoRa Pool | VRAM Pool | Name | Role |
|---|---|---|---|
| 1 | 0–4 region | SystemControl | OS Control & System Agent LoRa |
| 2 | 0–4 region | ReasoningLogic | Reasoning & Logic LoRa |
| 3 | 0–4 region | CodingSyntax | Coding & Syntax LoRa |
| 4 | 0–4 region | ConversationLang | Conversation & Language LoRa |

### Features

- **Zero-copy async hot-swapping:** Inactive LoRa weights live in Host Pinned Memory
  (`cudaMallocHost` / `hipHostMalloc`). Async loader uses `cudaMemcpyAsync` / `hipMemcpyAsync`
  on per-adapter CUDA/HIP Streams to pre-fetch the required adapter *before* the compute layer
  executes, avoiding GPU stalls. CPU fallback uses `std::memcpy` for CI/Kaggle.
- **SGMV-style weight patching:** `apply_lora_weights()` computes
  `output = base_output + B @ A @ input` on-the-fly without modifying the static base model
  weights in Pool 0. CPU fallback implements the full matrix multiply; GPU path provides
  device pointers for fused kernel dispatch.
- **LRU eviction:** `evict_lru_lora()` automatically evicts the Least Recently Used adapter
  back to System DDR RAM when VRAM is constrained, retaining only the active adapter(s) in
  Pools 1–4. `evict_all_except_active()` clears all cached adapters.
- **Thread-safety:** `LoRaRegistry` uses `std::shared_mutex` for registry state with
  per-adapter `std::mutex` for state transitions. `LoRaSwapper` uses `std::shared_mutex`
  for swap orchestration.
- **Adapter state machine:** `Unregistered → InDDR → Loading → Active → Evicting → InDDR`.
- **CUDA + ROCm dual-HAL:** 18 CUDA `#if` blocks ↔ 18 ROCm `#elif` blocks — perfectly
  balanced. `hipMemcpyAsync`, `hipHostMalloc`, `hipStreamCreate` etc. throughout.

### Python Binding

```python
import red_daft_lora as lora

lora.initialize()
lora.register_lora("coding_v2", 1024*1024, "/data/coding_v2.bin")  # 1 MB
lora.register_lora("reasoning_v1", 1024*1024, "/data/reasoning_v1.bin")
lora.swap_to_coding_lora("coding_v2")     # async host → VRAM
lora.swap_to_reasoning_lora("reasoning_v1")
lora.evict_lru()                           # evict coldest adapter
print(lora.lora_stats())                   # pool info, active adapter, DDR usage
lora.print_stats()                         # console summary
```

### Build + Test

```bash
# C++ tests
/tmp/vram-build/lora_test                  # 9 tests: register, hot-swap, LRU, SGMV, evict-all, etc.

# Python
RD_USE_CUDA=1 pip install -e ./vram-engine
python -c "import red_daft_lora; red_daft_lora.initialize(); red_daft_lora.print_stats()"
```

### 9-Test Smoke Coverage

| # | Test | What it proves |
|---|---|---|
| 1 | Register 4 adapters | All 4 LoRa pools accepted |
| 2 | Hot-swap sequence | Coding → Reasoning → System → Conversation (all active) |
| 3 | LRU victim | 5th adapter auto-evicts coldest |
| 4 | SGMV patching | `B @ A @ input` produces correct output |
| 5 | Empty adapter | 0-byte adapter handled gracefully |
| 6 | Host data integrity | Pinned host data survives alloc/free cycles |
| 7 | Stats | `lora_stats()` returns correct pool/adapter counts |
| 8 | Evict-all | All adapters returned to DDR |
| 9 | Raw registration | Unmanaged adapter registration works |

See `vram-engine/tests/test_lora.cpp`, `docs/ARCHITECTURE.md §3.3`.

## Nano-Context Engine — Ephemeral High-Density Context (Linux, C++20 + CUDA/HIP)

Keeps **Base Model Weights strictly in FP16 in VRAM Pool 0** and routes **all**
Input/Output Context Window + KV Cache allocations to dynamically spawned,
high-density **Nano-Pools**. Nano-Pools spawn on request start and recycle on
generation completion, with a hot free-list for low-latency re-spawn.

**Location:** `vram-engine/` — header `include/red_daft_nano_context.h`, impl
`src/red_daft_nano_context.cpp`, Python binding `src/nano_pybind_wrapper.cpp`.

### Core guarantees

- **Weights never move.** The Nano-Context Engine has no reference to VRAM Pool 0;
  it works only on its own ephemeral Nano-Pools.
- **FP16 → INT4 / INT2 KV quantization.** KV cache entries are per-channel
  quantized (scale + zero-point) inside the Nano-Pool buffer: up to **4–8× density**
  vs raw FP16. Base weights stay untouched at full FP16 precision.
- **Ephemeral Token Stream Ring** handles continuous input/output token traffic
  with a configurable footprint, default **< 50 MB per active stream**.
- **Spawn-on-request / recycle-on-completion** with a warm `free_list` (default 8
  recycled pools) to avoid allocator churn across requests.

### Python API

```python
import red_daft_nano_context as nano

cfg = nano.NanoContextConfig(kv_layers=32, kv_heads=8, head_dim=128,
                             max_tokens=4096, stream_capacity=24 << 20)
nano.nano_initialize(cfg)

rid = nano.request_begin()                     # spawn a Nano-Pool
key = [0.5] * 128;  val = [0.25] * 128         # flat lists, seq_tokens*head_dim
nano.kv_store(rid, layer=0, head=0, key=key, value=val, seq_tokens=1)
nano.stream_push(rid, [1.0, 2.0, 3.0])         # token stream
toks = nano.stream_pop(rid, 3)
print(nano.pool_stats(rid))                    # quant + stream stats
nano.request_end(rid)                          # recycle the Nano-Pool
```

### Native test

```bash
g++ -std=c++20 -Wall -Wextra -O3 -I vram-engine/include \
    vram-engine/src/red_daft_nano_context.cpp vram-engine/tests/test_nano.cpp \
    -o /tmp/nano_test
/tmp/nano_test      # KV INT4/INT2 round-trip, token stream, pool recycle → ALL PASS
```

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
| `iso.yml` | Full distro builds; hybrid BIOS+EFI ISO artifact (~430 MB) |
| `build.yml` | Container image, `daft-pkg` install, `ai-guard` v2, `vram_engine` CPU smoke (`vram_test` + `stress_all_pools`), `lora_test` (LoRa Brain Engine: 9 tests) |
| `gpu-compat.yml` | Kconfig audit ≥33/35, nvcc sm_86/89/120, hipcc gfx90a/gfx1100, fallback 25/25 |
| `ai-kernel.yml` | Demo Box **10 pools** — QEMU boot, 7/7 MATCH (incl. t-verify WRITEBACK), KV/scratch/10-pool, elasticity |
| `amd-rocm.yml` | Live-repo AMD package validation (rocm-runtime, amdgpu-dkms, rocm-dev, rocm-smi, miopen, pytorch-rocm, rocblas) + toolchain smoke |
| `nvidia-cuda.yml` | Live-repo NVIDIA package validation (cuda-drivers, cuda-toolkit, cudnn, tensorrt) + `nvcc` smoke |

## Directory layout
```
build/          ISO pipeline, hardened+GPU kernel config, Plymouth, assets gen
packages/       daft-pkg, deb-adapter, daft specs, gpu/ (daft-gpu + scorer), daft-kernel/
ai/             Modelfile, ollama-setup, daft-ai-guard v2 (+proxy, service)
shell/          daft-shell (in-RAM C/Python execution)
kernel/         daft-defmon LKM + ai-kernel/ (bare-metal Demo Box, 10 pools: HMM v3)
vram-engine/    C++20 tiered manager (+ LoRa + Nano-Context, ~5000 lines)
                  ├── include/red_daft_vram.h            10-pool VRAM engine header
                  ├── include/red_daft_lora_manager.h     LoRa Brain Engine header
                  ├── include/red_daft_nano_context.h     Nano-Context Engine header
                  ├── src/red_daft_vram.cpp               VRAM engine impl (10 pools, HAL)
                  ├── src/red_daft_lora_manager.cpp       LoRa engine impl (4 pools, SGMV, LRU)
                  ├── src/red_daft_nano_context.cpp       Nano-Context impl (INT4/INT2 KV, stream)
                  ├── src/pybind_wrapper.cpp              pybind11 VRAM module
                  ├── src/lora_pybind_wrapper.cpp         pybind11 LoRa module
                  ├── src/nano_pybind_wrapper.cpp         pybind11 Nano-Context module
                  ├── tests/test_vram.cpp                 10-pool smoke (PASS)
                  ├── tests/test_lora.cpp                 9 LoRa tests (ALL PASS)
                  ├── tests/test_nano.cpp                 Nano tests (ALL PASS)
                  ├── benchmarks/stress_3b.py             3B Kaggle/Linux stress
                  ├── CMakeLists.txt                      CMake (CUDA/ROCm/CPU)
                  └── setup.py                            pip install (auto-detect backend)
ux/             branding, MOTD, first-run ID card, installer
docs/           architecture (ARCHITECTURE.md)
.github/        ci workflows (iso, build, gpu-compat, ai-kernel, amd-rocm, nvidia-cuda)
```

## License

**Verbatim Distribution Only — No Derivatives Without Permission.**

- You **may** use the Software privately (including private local modifications that you do **not** distribute).
- You **may** reproduce and redistribute **verbatim, unmodified** copies of the Source Code and ISO images, **even commercially**, provided you retain all copyright/license notices and attribution to `Red Daft OS`.
- You **may not** create, publish, or distribute any modified version, fork, or derivative OS based on this Software to any third party, in source or binary form, without prior written consent from the authors.

See [`LICENSE`](LICENSE) for the full legal text (Red Daft OS Verbatim Distribution License v1.0).

> **Note on prior versions:** Commits prior to `2026-08-26` (`732a244` and earlier) remain available under `GPL-3.0-or-later` as originally licensed. Only versions published on or after that date are under the new verbatim-only license. The authors do not retroactively revoke GPL rights already granted.

For permission to distribute a modified version, open an issue or contact the authors via
`https://github.com/wippsanrinthailand80-commits/red-daft-os`.
