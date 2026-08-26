# Red Daft VRAM Engine — 10-Pool Tiered Memory Manager

High-performance, cross-platform **Custom VRAM Management Engine** for Red Daft OS. Strictly minimizes GPU VRAM while running/training LLMs (3B, 7B, 32B) by managing **10 distinct Memory Pools** and leveraging **System DDR4/DDR5 as a high-speed secondary tier** with asynchronous prefetching and dynamic offloading.

Built for **C++20 + CUDA VMM / ROCm HIP** with a **pybind11** binding for Kaggle/Linux `.so` benchmarks.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Red Daft VRAM Engine (10 Pools)              │
├─────┬──────────────────────────┬──────────────┬──────────────────┤
│ Pool│ Name                     │ Policy       │ Quota (2 GiB)    │
├─────┼──────────────────────────┼──────────────┼──────────────────┤
│ 0   │ ModelWeights             │ LRU_FREQ     │ 35%  716 MiB    │
│ 1   │ KvCache                  │ ARENA        │ 25%  512 MiB    │
│ 2   │ ActivationsTensors       │ FIFO         │ 15%  307 MiB    │
│ 3   │ WorkspaceScratchpad      │ FIFO         │  5%  102 MiB    │
│ 4   │ HostSwapStaging          │ LRU_GENERIC  │  5%  102 MiB    │
│ 5   │ EmbeddingBuffers         │ LRU_FREQ     │  5%  102 MiB    │
│ 6   │ QuantizationMetadata     │ LRU_GENERIC  │  3%   61 MiB    │
│ 7   │ AsyncStreamQueue         │ LRU_GENERIC  │  2%   40 MiB    │
│ 8   │ SystemIpcShared          │ LRU_GENERIC  │  3%   61 MiB    │
│ 9   │ EmergencyOverflow        │ LRU_GENERIC  │  2%   40 MiB    │  ← lends to 1/2
└─────┴──────────────────────────┴──────────────┴──────────────────┘
         Tier 0: GPU VRAM (active)  ◄──►  Tier 1: Host DDR pinned (cold)
                         cudaMemcpyAsync / hipMemcpyAsync on 4 streams
                         double-buffered prefetch hides PCIe latency
```

### Tiered Memory Management

* **High-Speed Tier (VRAM):** active compute tensors, current layer weights, immediate KV window. Allocated via `cudaMalloc` / `hipMalloc`.
* **Capacity Tier (DDR):** cold weights + long-context KV, stored in **pinned host memory** (`cudaMallocHost` / `hipHostMalloc`) for high-bandwidth DMA.
* **Async Pipeline:** every `prefetch_to_vram()` / `offload_to_ddr()` is `cudaMemcpyAsync` on a dedicated `cudaStream_t` (4 streams, round-robin). Optional **double-buffering** (`shadow_ptr`) stages the next layer while the current computes, preventing GPU stalls.

### Hardware Abstraction Layer (HAL)

Macro-switchable header:

```bash
# NVIDIA (default if cuda_runtime.h found)
-DRD_USE_CUDA   # → cudaMalloc / cudaMemcpyAsync / cudaStream_t

# AMD ROCm
-DRD_USE_ROCM   # → hipMalloc / hipMemcpyAsync / hipStream_t

# CPU fallback (CI, Kaggle CPU): no flag → malloc + memcpy simulation
```

All device/host alloc, free, memcpy, streams go through `hal_*` wrappers in `src/red_daft_vram.cpp`.

### Dynamic Borrowing & Anti-Starvation

* If `Pool 1 (KvCache)` or `Pool 2 (Activations)` pressure `>85%`, `borrow_memory()` tries to **lend** `bytes` from `Pool 9 (Emergency)`.
* If `Emergency` also pressured, it first **offloads its own LRU cold blocks** to host to make room (`offload_lru_to_host`), then retries.
* Hysteresis: borrow only above `high_pressure_threshold` (85%), return below `low_pressure_threshold` (60%) on `deallocate()`.
* `KvCache` is `ARENA` (never auto-evicted) — OOM is reported, not silently dropped (real vLLM semantics).

---

## Deliverables

| File | Purpose |
|------|---------|
| `include/red_daft_vram.h` | Fully defined `enum class PoolType` (10), `MemoryBlock`, `TieredBuffer`, `MemoryPool`, `VramEngine`, `EngineConfig`, `PoolStats`, HAL types. C++20, thread-safe (`std::mutex`/`shared_mutex`). |
| `src/red_daft_vram.cpp` | `initialize()`, `allocate()`, `deallocate()`, `prefetch_to_vram()`, `offload_to_ddr()`, `borrow_memory()`, `offload_lru_to_host()`, `print_pool_stats()`, double-buffering, borrowing engine. |
| `src/pybind_wrapper.cpp` | `pybind11` module `red_daft_vram` + optional `torch/extension.h` integration + `stress_3b_benchmark()` + `allocate_torch()`. |

---

## Build

### CMake (C++ + Python)

```bash
mkdir build && cd build
cmake .. -DUSE_CUDA=ON   # or -DUSE_ROCM=ON, or -DUSE_CUDA=OFF for CPU fallback
make -j
./vram_test              # native C++ smoke
python3 -c "import red_daft_vram; red_daft_vram.stress_3b_benchmark(verbose=True)"
```

### pip (Kaggle / Linux)

```bash
pip install pybind11
pip install -e .                    # CPU fallback (still benchmarks)
RD_USE_CUDA=1 pip install -e .      # CUDA
RD_USE_ROCM=1 pip install -e .      # ROCm
```

### One-liner Kaggle

```bash
!git clone https://github.com/wippsanrinthailand80-commits/red-daft-os.git
!cd red-daft-os/vram-engine && pip install pybind11 && pip install -e . && python benchmarks/stress_3b.py
```

---

## Python API

```python
import red_daft_vram as rdv

# Initialize (auto-detect GPU or CPU fallback)
rdv.initialize()  # or rdv.initialize(rdv.EngineConfig(vram_budget_bytes=2<<30))

# Allocate — returns Block (RAII) or raw handle
blk = rdv.allocate(0, 1<<20, tag="layer_0_qkv")  # Pool 0, 1 MiB
h   = rdv.allocate_handle(1, 4096, tag="kv_page")  # Pool 1, raw handle

rdv.prefetch_to_vram(h, double_buffer=True)  # host→VRAM async
rdv.offload_to_ddr(h, keep_vram_copy=False) # VRAM→DDR async

# Borrowing
rdv.borrow_memory(1, 1<<20)  # try to lend 1 MiB from Emergency to KV

# Stats
print(rdv.get_pool_stats(0))
rdv.print_pool_stats()

# Torch integration (if torch installed)
import torch
t = rdv.allocate_torch(pool=0, shape=[3072, 3072], dtype="bf16")

# Stress benchmarks
print(rdv.stress_all_pools())  # 1 MiB per pool
res = rdv.stress_3b_benchmark(layers=28, hidden=3072, seq_len=2048, iterations=3, verbose=True)
print(res.report)
# BenchmarkResult: total_seconds, peak_vram_mib, avg_alloc_us, emergency_borrows, passed
```

---

## Benchmarks

```bash
python benchmarks/stress_3b.py --layers 28 --hidden 3072 --seq-len 2048 --iters 3
```

Simulates a 3B model (28 layers) streaming weights through Pool 0 with double-buffered prefetch, KV paged expansion in Pool 1 (128-token pages), activation churn in Pool 2 (FIFO), workspace scratch in Pool 3, and emergency borrowing under pressure. Reports peak VRAM, avg alloc latency, offloads/prefetches, borrows.

---

## Integration with Red Daft OS

* **Bare-metal AI-Kernel** (`kernel/ai-kernel/`): the kernel's HMM already exposes 10 pools (0..9) via `hmm_register_model_p(..., pool)` and `kernel pool` CLI. The VRAM engine is the **Linux counterpart** for CUDA/HIP.
* **ISO:** `daft-kernel` + `red_daft_vram` are installed to `/opt/daft/vram-engine` and `/usr/local/lib`.

---

## License

Red Daft OS Verbatim Distribution License v1.0 — see `/LICENSE`.

Verbatim redistribution (even commercially) is permitted with attribution;
private local modifications are allowed but must not be distributed;
public forks / derivative OS distributions require prior written consent.
Prior commits (≤ `732a244`) remain available under GPL-3.0-or-later.
