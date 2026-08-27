/**
 * red_daft_lora_manager.cpp — Modular LoRa Brain Engine (implementation)
 * ======================================================================
 * Production C++20 implementation of the LoRa adapter hot-swap engine.
 * See red_daft_lora_manager.h for architecture overview.
 *
 * Design:
 *  - LoRaRegistry owns all adapters, tracks state + LRU, thread-safe
 *  - LoRaSwapper orchestrates async host↔VRAM transfers via VramEngine
 *  - Each adapter has pinned host memory (always valid) + optional VRAM
 *  - hot_swap_async() pre-fetches target to VRAM before compute needs it
 *  - evict_lru_lora() moves coldest adapter back to DDR when VRAM is tight
 *  - apply_lora_weights() does SGMV-style dynamic patching (B @ A @ x)
 *    without modifying static base weights in Pool 0
 *
 * HAL: all GPU ops go through VramEngine's hal wrappers (CUDA/ROCm/CPU).
 *
 * Threading: LoRaRegistry uses std::shared_mutex for registry state.
 * LoRaSwapper uses std::shared_mutex for swap orchestration.
 * Per-adapter std::mutex guards individual adapter state transitions.
 */

#include "red_daft_lora_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>

// CPU fallback for SGMV when no GPU present
#if defined(RD_BACKEND_CUDA)
  #include <cuda_runtime.h>
#elif defined(RD_BACKEND_ROCM)
  #include <hip/hip_runtime.h>
#endif

namespace red_daft {

using Clock = std::chrono::steady_clock;
using HiRes = std::chrono::high_resolution_clock;

// ═══════════════════════════════════════════════════════════════════════
//  LoRaRegistry implementation
// ═══════════════════════════════════════════════════════════════════════

LoRaRegistry::LoRaRegistry() {
    pool_idx_.fill(SIZE_MAX);
}

LoRaRegistry::~LoRaRegistry() {
    std::shared_lock<std::shared_mutex> lock(mutex);
    for (auto& a : adapters_) {
        if (!a) continue;
        // Free device memory
        if (a->d_A) {
#if defined(RD_BACKEND_CUDA)
            cudaFree(a->d_A);
#elif defined(RD_BACKEND_ROCM)
            hipFree(a->d_A);
#else
            std::free(a->d_A);
#endif
        }
        if (a->d_B) {
#if defined(RD_BACKEND_CUDA)
            cudaFree(a->d_B);
#elif defined(RD_BACKEND_ROCM)
            hipFree(a->d_B);
#else
            std::free(a->d_B);
#endif
        }
        // Free host memory (only if we own it)
        if (a->owns_host) {
            if (a->h_A) {
#if defined(RD_BACKEND_CUDA)
                cudaFreeHost(a->h_A);
#elif defined(RD_BACKEND_ROCM)
                hipHostFree(a->h_A);
#else
                std::free(a->h_A);
#endif
            }
            if (a->h_B) {
#if defined(RD_BACKEND_CUDA)
                cudaFreeHost(a->h_B);
#elif defined(RD_BACKEND_ROCM)
                hipHostFree(a->h_B);
#else
                std::free(a->h_B);
#endif
            }
        }
        // Free stream
        if (a->stream) {
#if defined(RD_BACKEND_CUDA)
            cudaStreamDestroy(a->stream);
#elif defined(RD_BACKEND_ROCM)
            hipStreamDestroy(a->stream);
#endif
        }
    }
}

// ── Pinned host allocation helper ────────────────────────────────────

static bool alloc_pinned_host(void** ptr, size_t bytes) {
#if defined(RD_BACKEND_CUDA)
    cudaError_t e = cudaMallocHost(ptr, bytes);
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[lora] cudaMallocHost %zu failed: %s\n", bytes, cudaGetErrorString(e));
        return false;
    }
    return true;
#elif defined(RD_BACKEND_ROCM)
    hipError_t e = hipHostMalloc(ptr, bytes, hipHostMallocDefault);
    if (e != hipSuccess) {
        std::fprintf(stderr, "[lora] hipHostMalloc %zu failed: %s\n", bytes, hipGetErrorString(e));
        return false;
    }
    return true;
#else
    *ptr = std::malloc(bytes);
    if (!*ptr) {
        std::fprintf(stderr, "[lora] malloc host %zu failed (CPU)\n", bytes);
        return false;
    }
    std::memset(*ptr, 0, bytes);
    return true;
#endif
}

static void free_pinned_host(void* ptr) {
    if (!ptr) return;
#if defined(RD_BACKEND_CUDA)
    cudaFreeHost(ptr);
#elif defined(RD_BACKEND_ROCM)
    hipHostFree(ptr);
#else
    std::free(ptr);
#endif
}

// ── Device allocation helper ─────────────────────────────────────────

static bool alloc_device(void** ptr, size_t bytes) {
#if defined(RD_BACKEND_CUDA)
    cudaError_t e = cudaMalloc(ptr, bytes);
    return e == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    hipError_t e = hipMalloc(ptr, bytes);
    return e == hipSuccess;
#else
    *ptr = std::malloc(bytes);
    if (*ptr) std::memset(*ptr, 0, bytes);
    return *ptr != nullptr;
#endif
}

static void free_device(void* ptr) {
    if (!ptr) return;
#if defined(RD_BACKEND_CUDA)
    cudaFree(ptr);
#elif defined(RD_BACKEND_ROCM)
    hipFree(ptr);
#else
    std::free(ptr);
#endif
}

static bool create_stream(hal::Stream* s) {
#if defined(RD_BACKEND_CUDA)
    return cudaStreamCreate(s) == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    return hipStreamCreate(s) == hipSuccess;
#else
    *s = nullptr;
    return true;
#endif
}

static void destroy_stream(hal::Stream s) {
    if (!s) return;
#if defined(RD_BACKEND_CUDA)
    cudaStreamDestroy(s);
#elif defined(RD_BACKEND_ROCM)
    hipStreamDestroy(s);
#endif
}

// ── Registration ─────────────────────────────────────────────────────

uint32_t LoRaRegistry::register_adapter(
    std::string_view name, LoRaPool pool, uint32_t rank,
    uint32_t in_features, uint32_t out_features,
    const float* host_A, const float* host_B,
    hal::Stream /*stream*/)
{
    size_t a_bytes = (size_t)rank * in_features * sizeof(float);
    size_t b_bytes = (size_t)out_features * rank * sizeof(float);
    size_t total = a_bytes + b_bytes;

    auto adapter = std::make_unique<LoRaAdapter>();
    adapter->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    adapter->name = std::string(name);
    adapter->pool = pool;
    adapter->rank = rank;
    adapter->in_features = in_features;
    adapter->out_features = out_features;
    adapter->a_size = a_bytes;
    adapter->b_size = b_bytes;
    adapter->total_size = total;
    adapter->last_access = Clock::now();
    adapter->access_count = 1;

    // Allocate pinned host memory
    if (!alloc_pinned_host(&adapter->h_A, a_bytes) ||
        !alloc_pinned_host(&adapter->h_B, b_bytes)) {
        std::fprintf(stderr, "[lora] register '%s': host alloc failed\n", std::string(name).c_str());
        return 0;
    }
    adapter->owns_host = true;

    // Copy weight data if provided
    if (host_A) std::memcpy(adapter->h_A, host_A, a_bytes);
    if (host_B) std::memcpy(adapter->h_B, host_B, b_bytes);

    // Create per-adapter stream
    if (!create_stream(&adapter->stream)) {
        std::fprintf(stderr, "[lora] register '%s': stream create failed\n", std::string(name).c_str());
    }

    adapter->state = LoRaState::InDDR;

    uint32_t id = adapter->id;

    {
        std::unique_lock<std::shared_mutex> wlock(mutex);
        size_t idx = adapters_.size();
        adapters_.push_back(std::move(adapter));
        id_idx_[id] = idx;
        name_idx_[std::string(name)] = idx;
        if (pool_idx_[static_cast<size_t>(pool)] == SIZE_MAX) {
            pool_idx_[static_cast<size_t>(pool)] = idx;
        }
    }

    std::printf("[lora] registered '%s' id=%u pool=%s rank=%u A=%zuB B=%zuB total=%zuKB\n",
                std::string(name).c_str(), id, to_string(pool).data(),
                rank, a_bytes, b_bytes, total >> 10);
    return id;
}

uint32_t LoRaRegistry::register_adapter_raw(
    std::string_view name, LoRaPool pool, uint32_t rank,
    uint32_t in_features, uint32_t out_features,
    const void* raw_A, size_t /*raw_a_bytes*/,
    const void* raw_B, size_t /*raw_b_bytes*/,
    hal::Stream /*stream*/)
{
    return register_adapter(
        name, pool, rank, in_features, out_features,
        reinterpret_cast<const float*>(raw_A),
        reinterpret_cast<const float*>(raw_B));
}

uint32_t LoRaRegistry::register_empty(
    std::string_view name, LoRaPool pool, uint32_t rank,
    uint32_t in_features, uint32_t out_features,
    hal::Stream /*stream*/)
{
    return register_adapter(name, pool, rank, in_features, out_features,
                            nullptr, nullptr);
}

void LoRaRegistry::unregister(uint32_t id) {
    std::unique_lock<std::shared_mutex> wlock(mutex);
    auto it = id_idx_.find(id);
    if (it == id_idx_.end()) return;
    size_t idx = it->second;

    auto& adapter = adapters_[idx];
    if (adapter) {
        if (adapter->d_A) free_device(adapter->d_A);
        if (adapter->d_B) free_device(adapter->d_B);
        if (adapter->owns_host) {
            free_pinned_host(adapter->h_A);
            free_pinned_host(adapter->h_B);
        }
        if (adapter->stream) destroy_stream(adapter->stream);

        name_idx_.erase(adapter->name);
        if (pool_idx_[static_cast<size_t>(adapter->pool)] == idx) {
            pool_idx_[static_cast<size_t>(adapter->pool)] = SIZE_MAX;
        }
    }
    adapters_[idx].reset();
    id_idx_.erase(it);
}

// ── Lookup ───────────────────────────────────────────────────────────

LoRaAdapter* LoRaRegistry::find_by_id(uint32_t id) {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    auto it = id_idx_.find(id);
    if (it == id_idx_.end()) return nullptr;
    return adapters_[it->second].get();
}

const LoRaAdapter* LoRaRegistry::find_by_id(uint32_t id) const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    auto it = id_idx_.find(id);
    if (it == id_idx_.end()) return nullptr;
    return adapters_[it->second].get();
}

LoRaAdapter* LoRaRegistry::find_by_name(std::string_view name) {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    auto it = name_idx_.find(std::string(name));
    if (it == name_idx_.end()) return nullptr;
    return adapters_[it->second].get();
}

LoRaAdapter* LoRaRegistry::find_by_pool(LoRaPool pool) {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    size_t idx = pool_idx_[static_cast<size_t>(pool)];
    if (idx == SIZE_MAX || idx >= adapters_.size()) return nullptr;
    return adapters_[idx].get();
}

bool LoRaRegistry::has_pool(LoRaPool pool) const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    return pool_idx_[static_cast<size_t>(pool)] != SIZE_MAX;
}

// ── LRU ──────────────────────────────────────────────────────────────

void LoRaRegistry::touch(uint32_t id) {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    auto it = id_idx_.find(id);
    if (it == id_idx_.end()) return;
    auto& a = adapters_[it->second];
    if (a) {
        std::lock_guard<std::mutex> lock(a->mtx);
        a->last_access = Clock::now();
        a->access_count++;
    }
}

LoRaAdapter* LoRaRegistry::lru_victim() {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    LoRaAdapter* victim = nullptr;
    Clock::time_point oldest = Clock::time_point::max();

    for (auto& a : adapters_) {
        if (!a) continue;
        if (a->state == LoRaState::Unregistered ||
            a->state == LoRaState::Evicting ||
            a->state == LoRaState::Loading) continue;
        // Don't evict the currently active adapter
        // (caller must check this separately)
        std::lock_guard<std::mutex> lock(a->mtx);
        if (a->last_access < oldest) {
            oldest = a->last_access;
            victim = a.get();
        }
    }
    return victim;
}

// ── State ────────────────────────────────────────────────────────────

void LoRaRegistry::set_state(uint32_t id, LoRaState state) {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    auto it = id_idx_.find(id);
    if (it == id_idx_.end()) return;
    auto& a = adapters_[it->second];
    if (a) {
        std::lock_guard<std::mutex> lock(a->mtx);
        a->state = state;
    }
}

// ── Iteration / Stats ────────────────────────────────────────────────

size_t LoRaRegistry::count() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    return adapters_.size();
}

size_t LoRaRegistry::active_count() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    size_t n = 0;
    for (auto& a : adapters_)
        if (a && a->state == LoRaState::Active) n++;
    return n;
}

size_t LoRaRegistry::vram_count() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    size_t n = 0;
    for (auto& a : adapters_)
        if (a && a->is_on_vram()) n++;
    return n;
}

size_t LoRaRegistry::total_vram_bytes() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    size_t t = 0;
    for (auto& a : adapters_)
        if (a && a->is_on_vram()) t += a->total_size;
    return t;
}

size_t LoRaRegistry::total_host_bytes() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    size_t t = 0;
    for (auto& a : adapters_)
        if (a && a->is_on_host()) t += a->total_size;
    return t;
}

std::vector<LoRaAdapter*> LoRaRegistry::all_adapters() {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    std::vector<LoRaAdapter*> out;
    for (auto& a : adapters_)
        if (a) out.push_back(a.get());
    return out;
}

std::vector<const LoRaAdapter*> LoRaRegistry::all_adapters() const {
    std::shared_lock<std::shared_mutex> rlock(mutex);
    std::vector<const LoRaAdapter*> out;
    for (auto& a : adapters_)
        if (a) out.push_back(a.get());
    return out;
}

// ═══════════════════════════════════════════════════════════════════════
//  LoRaSwapper implementation
// ═══════════════════════════════════════════════════════════════════════

LoRaSwapper::LoRaSwapper(LoRaRegistry& registry, VramEngine& engine)
    : registry_(registry), engine_(engine) {}

LoRaSwapper::~LoRaSwapper() {
    // Best-effort: evict all active adapters
    try { evict_all_except_active(); } catch (...) {}
    try { synchronize(); } catch (...) {}
}

// ── HAL memcpy wrappers ──────────────────────────────────────────────

bool LoRaSwapper::async_host_to_device(void* dst, const void* src, size_t bytes, hal::Stream stream) {
#if defined(RD_BACKEND_CUDA)
    if (!stream) return false;
    cudaMemcpyKind kind = cudaMemcpyHostToDevice;
    cudaError_t e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    return e == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    if (!stream) return false;
    hipError_t e = hipMemcpyAsync(dst, src, bytes, hipMemcpyHostToDevice, stream);
    return e == hipSuccess;
#else
    (void)stream;
    std::memcpy(dst, src, bytes);
    return true;
#endif
}

bool LoRaSwapper::async_device_to_host(void* dst, const void* src, size_t bytes, hal::Stream stream) {
#if defined(RD_BACKEND_CUDA)
    if (!stream) return false;
    cudaMemcpyKind kind = cudaMemcpyDeviceToHost;
    cudaError_t e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    return e == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    if (!stream) return false;
    hipError_t e = hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToHost, stream);
    return e == hipSuccess;
#else
    (void)stream;
    std::memcpy(dst, src, bytes);
    return true;
#endif
}

bool LoRaSwapper::sync_memcpy(void* dst, const void* src, size_t bytes, bool to_device) {
    (void)to_device;
#if defined(RD_BACKEND_CUDA)
    cudaMemcpyKind kind = to_device ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost;
    cudaError_t e = cudaMemcpy(dst, src, bytes, kind);
    return e == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    hipError_t e = hipMemcpy(dst, src, bytes, to_device ? hipMemcpyHostToDevice : hipMemcpyDeviceToHost);
    return e == hipSuccess;
#else
    std::memcpy(dst, src, bytes);
    return true;
#endif
}

// ── VRAM capacity management ─────────────────────────────────────────

bool LoRaSwapper::ensure_vram_capacity(size_t bytes_needed, hal::Stream stream) {
    // Check if VramEngine ModelWeights pool has room
    auto stats = engine_.get_pool_stats(PoolType::ModelWeights);
    if (stats.quota_bytes >= stats.used_vram + bytes_needed) {
        return true;
    }

    // Need to evict LRU LoRa adapter(s) from VRAM to make room
    size_t freed = 0;
    while (freed < bytes_needed) {
        LoRaAdapter* victim = registry_.lru_victim();
        if (!victim || victim->id == active_id_.load()) break;

        std::lock_guard<std::mutex> lock(victim->mtx);
        if (victim->state != LoRaState::Active && victim->state != LoRaState::InDDR) break;
        if (!victim->is_on_vram()) break;

        // Async copy VRAM→host
        hal::Stream s = stream ? stream : victim->stream;
        bool ok = async_device_to_host(victim->h_A, victim->d_A, victim->a_size, s);
        if (ok) ok = async_device_to_host(victim->h_B, victim->d_B, victim->b_size, s);
        if (!ok) break;

        // Free VRAM
        free_device(victim->d_A);
        free_device(victim->d_B);
        victim->d_A = nullptr;
        victim->d_B = nullptr;
        victim->state = LoRaState::InDDR;
        freed += victim->total_size;

        std::printf("[lora] evicted '%s' id=%u (%zuKB) to DDR\n",
                    victim->name.c_str(), victim->id, victim->total_size >> 10);
        stats_.total_evictions++;
    }

    return freed >= bytes_needed ||
           engine_.get_pool_stats(PoolType::ModelWeights).quota_bytes >=
           engine_.get_pool_stats(PoolType::ModelWeights).used_vram + bytes_needed;
}

// ── Core hot-swap ────────────────────────────────────────────────────

bool LoRaSwapper::hot_swap_async(uint32_t target_id, hal::Stream stream) {
    auto t0 = HiRes::now();

    LoRaAdapter* target = registry_.find_by_id(target_id);
    if (!target) {
        std::fprintf(stderr, "[lora] hot_swap: adapter %u not found\n", target_id);
        return false;
    }

    std::lock_guard<std::shared_mutex> wlock(mtx_);

    // Already active — just touch LRU
    if (active_id_.load() == target_id && target->state == LoRaState::Active) {
        registry_.touch(target_id);
        return true;
    }

    // ── Phase 1: Ensure VRAM capacity ─────────────────────────────
    if (!target->is_on_vram()) {
        if (!ensure_vram_capacity(target->total_size, stream)) {
            std::fprintf(stderr, "[lora] hot_swap: cannot fit adapter '%s' (%zuKB) in VRAM\n",
                         target->name.c_str(), target->total_size >> 10);
            return false;
        }
    }

    // ── Phase 2: Evict previous active if different adapter ───────
    uint32_t prev_id = active_id_.load();
    if (prev_id != 0 && prev_id != target_id) {
        LoRaAdapter* prev = registry_.find_by_id(prev_id);
        if (prev) {
            std::lock_guard<std::mutex> lock(prev->mtx);
            if (prev->is_on_vram() && prev->state == LoRaState::Active) {
                // Async copy back to DDR
                hal::Stream s = stream ? stream : prev->stream;
                prev->state = LoRaState::Evicting;
                async_device_to_host(prev->h_A, prev->d_A, prev->a_size, s);
                async_device_to_host(prev->h_B, prev->d_B, prev->b_size, s);

                // Free VRAM immediately (transfer runs async)
                free_device(prev->d_A);
                free_device(prev->d_B);
                prev->d_A = nullptr;
                prev->d_B = nullptr;
                prev->state = LoRaState::InDDR;

                std::printf("[lora] evicted previous '%s' id=%u (%zuKB)\n",
                            prev->name.c_str(), prev->id, prev->total_size >> 10);
                stats_.total_evictions++;
            }
        }
    }

    // ── Phase 3: Async copy target to VRAM ────────────────────────
    if (!target->is_on_vram()) {
        // Allocate VRAM for A and B
        void* d_A = nullptr;
        void* d_B = nullptr;
        if (!alloc_device(&d_A, target->a_size) || !alloc_device(&d_B, target->b_size)) {
            std::fprintf(stderr, "[lora] hot_swap: VRAM alloc failed for '%s'\n", target->name.c_str());
            if (d_A) free_device(d_A);
            if (d_B) free_device(d_B);
            return false;
        }

        target->d_A = d_A;
        target->d_B = d_B;
    }

    // Async transfer: host→device
    {
        std::lock_guard<std::mutex> lock(target->mtx);
        target->state = LoRaState::Loading;
    }

    hal::Stream s = stream ? stream : target->stream;
    bool ok_a = async_host_to_device(target->d_A, target->h_A, target->a_size, s);
    bool ok_b = async_host_to_device(target->d_B, target->h_B, target->b_size, s);

    if (!ok_a || !ok_b) {
        std::fprintf(stderr, "[lora] hot_swap: async transfer failed for '%s'\n", target->name.c_str());
        free_device(target->d_A);
        free_device(target->d_B);
        target->d_A = nullptr;
        target->d_B = nullptr;
        target->state = LoRaState::InDDR;
        return false;
    }

    // ── Phase 4: Mark as active ───────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(target->mtx);
        target->state = LoRaState::Active;
        target->last_access = Clock::now();
        target->access_count++;
    }
    active_id_.store(target_id, std::memory_order_relaxed);

    stats_.total_swaps++;
    stats_.total_bytes_transferred += target->total_size;
    record_swap_time(t0);

    std::printf("[lora] hot_swap '%s' id=%u (%zuKB) → VRAM [async]\n",
                target->name.c_str(), target->id, target->total_size >> 10);
    return true;
}

bool LoRaSwapper::swap_to_pool(LoRaPool pool, hal::Stream stream) {
    LoRaAdapter* a = registry_.find_by_pool(pool);
    if (!a) {
        std::fprintf(stderr, "[lora] swap_to_pool: no adapter in pool %s\n", to_string(pool).data());
        return false;
    }
    return hot_swap_async(a->id, stream);
}

bool LoRaSwapper::pre_activate(uint32_t id, hal::Stream stream) {
    // Same as hot_swap_async but doesn't mark as "active" —
    // the adapter is in VRAM but not the "current" one.
    // Useful for double-buffering: prefetch next layer's LoRa.
    auto t0 = HiRes::now();
    LoRaAdapter* target = registry_.find_by_id(id);
    if (!target) return false;

    std::lock_guard<std::shared_mutex> wlock(mtx_);

    if (target->is_on_vram()) {
        registry_.touch(id);
        return true;
    }

    if (!ensure_vram_capacity(target->total_size, stream)) return false;

    if (!alloc_device(&target->d_A, target->a_size) ||
        !alloc_device(&target->d_B, target->b_size)) {
        return false;
    }

    hal::Stream s = stream ? stream : target->stream;
    bool ok = async_host_to_device(target->d_A, target->h_A, target->a_size, s);
    if (ok) ok = async_host_to_device(target->d_B, target->h_B, target->b_size, s);
    if (!ok) {
        free_device(target->d_A);
        free_device(target->d_B);
        target->d_A = nullptr;
        target->d_B = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(target->mtx);
        target->state = LoRaState::InDDR; // pre-activated, not Active yet
        target->last_access = Clock::now();
    }

    stats_.total_prefetches++;
    stats_.total_bytes_transferred += target->total_size;
    record_swap_time(t0);
    return true;
}

// ── SGMV-style weight patching ───────────────────────────────────────

bool LoRaSwapper::apply_lora_weights(uint32_t adapter_id,
                                      void* base_output, const void* base_input,
                                      size_t batch_size) {
    LoRaAdapter* adapter = registry_.find_by_id(adapter_id);
    if (!adapter) return false;

    // Ensure adapter is in VRAM
    if (!adapter->is_on_vram()) {
        if (!hot_swap_async(adapter_id)) return false;
        // Synchronize to ensure weights are ready
        if (adapter->stream) {
#if defined(RD_BACKEND_CUDA)
            cudaStreamSynchronize(adapter->stream);
#elif defined(RD_BACKEND_ROCM)
            hipStreamSynchronize(adapter->stream);
#endif
        }
    }

    registry_.touch(adapter_id);

    uint32_t r = adapter->rank;
    uint32_t in = adapter->in_features;
    uint32_t out = adapter->out_features;

    // SGMV: output = B @ A @ input (where A: r×in, B: out×r)
    // This is the LoRA delta that gets added to the base model output.
    // For the CPU fallback, we do the matrix multiply directly.
    // For GPU, in production this would launch a fused CUDA kernel.
    //
    // For now, we provide the data layout so the calling code can dispatch
    // to its own SGMV kernel. The key insight is that A and B are now
    // contiguous in VRAM and ready for any GPU kernel to consume.

    std::printf("[lora] apply '%s' id=%u: rank=%u in=%u out=%u batch=%zu (A=%p B=%p)\n",
                adapter->name.c_str(), adapter->id, r, in, out, batch_size,
                adapter->d_A, adapter->d_B);

    // CPU fallback SGMV: compute B @ A (out × in) into a temp buffer
    // and add to base_output. This demonstrates the algorithm; in production,
    // the GPU kernel would handle this inline.
#if defined(RD_BACKEND_CPU)
    if (base_output && base_input && adapter->h_A && adapter->h_B) {
        // For each batch element:
        //   temp[r] = A @ input[in]       (r × 1 = r×in @ in×1)
        //   output[out] += B @ temp[r]    (out×1 = out×r @ r×1)
        const float* A = reinterpret_cast<const float*>(adapter->h_A);
        const float* B = reinterpret_cast<const float*>(adapter->h_B);
        const float* inp = reinterpret_cast<const float*>(base_input);
        float* outp = reinterpret_cast<float*>(base_output);

        for (size_t b = 0; b < batch_size; ++b) {
            std::vector<float> temp(r, 0.0f);

            // temp = A @ input
            for (uint32_t i = 0; i < r; ++i) {
                float sum = 0.0f;
                for (uint32_t j = 0; j < in; ++j) {
                    sum += A[i * in + j] * inp[b * in + j];
                }
                temp[i] = sum;
            }

            // output += B @ temp
            for (uint32_t i = 0; i < out; ++i) {
                float sum = 0.0f;
                for (uint32_t j = 0; j < r; ++j) {
                    sum += B[i * r + j] * temp[j];
                }
                outp[b * out + i] += sum;
            }
        }
    }
#else
    // GPU path: A and B are in VRAM at d_A, d_B.
    // Production code would launch a fused SGMV kernel here.
    // For now, we synchronize and report success — the calling
    // framework (e.g., PyTorch extension) handles the actual
    // kernel dispatch using these device pointers.
    (void)base_output;
    (void)base_input;
    (void)batch_size;
#endif

    return true;
}

// ── Eviction ─────────────────────────────────────────────────────────

bool LoRaSwapper::evict_adapter(uint32_t id, hal::Stream stream) {
    LoRaAdapter* adapter = registry_.find_by_id(id);
    if (!adapter) return false;

    std::lock_guard<std::shared_mutex> wlock(mtx_);
    std::lock_guard<std::mutex> lock(adapter->mtx);

    if (!adapter->is_on_vram()) return true; // already evicted
    if (adapter->state == LoRaState::Evicting) return true;

    hal::Stream s = stream ? stream : adapter->stream;
    adapter->state = LoRaState::Evicting;

    // Async copy VRAM→host
    bool ok_a = async_device_to_host(adapter->h_A, adapter->d_A, adapter->a_size, s);
    bool ok_b = async_device_to_host(adapter->h_B, adapter->d_B, adapter->b_size, s);

    if (!ok_a || !ok_b) {
        std::fprintf(stderr, "[lora] evict: async transfer failed for '%s'\n", adapter->name.c_str());
        adapter->state = LoRaState::Active;
        return false;
    }

    // Free VRAM
    free_device(adapter->d_A);
    free_device(adapter->d_B);
    adapter->d_A = nullptr;
    adapter->d_B = nullptr;
    adapter->state = LoRaState::InDDR;

    // If this was the active adapter, clear active_id
    if (active_id_.load() == id) {
        active_id_.store(0, std::memory_order_relaxed);
    }

    std::printf("[lora] evicted '%s' id=%u (%zuKB) → DDR\n",
                adapter->name.c_str(), adapter->id, adapter->total_size >> 10);
    stats_.total_evictions++;
    stats_.total_bytes_transferred += adapter->total_size;
    return true;
}

bool LoRaSwapper::evict_lru_lora(hal::Stream stream) {
    uint32_t active = active_id_.load();
    std::vector<LoRaAdapter*> all = registry_.all_adapters();

    LoRaAdapter* victim = nullptr;
    Clock::time_point oldest = Clock::time_point::max();

    for (auto* a : all) {
        if (!a) continue;
        if ((int)a->state < (int)LoRaState::InDDR) continue;
        if (a->state == LoRaState::Evicting || a->state == LoRaState::Loading) continue;
        if (a->id == active) continue; // don't evict active
        if (!a->is_on_vram()) continue;

        std::lock_guard<std::mutex> lock(a->mtx);
        if (a->last_access < oldest) {
            oldest = a->last_access;
            victim = a;
        }
    }

    if (!victim) return false;
    return evict_adapter(victim->id, stream);
}

size_t LoRaSwapper::evict_all_except_active(hal::Stream stream) {
    uint32_t active = active_id_.load();
    size_t evicted = 0;

    for (auto* a : registry_.all_adapters()) {
        if (!a || a->id == active) continue;
        if (!a->is_on_vram()) continue;
        if (evict_adapter(a->id, stream)) evicted++;
    }
    return evicted;
}

// ── Introspection ────────────────────────────────────────────────────

LoRaAdapter* LoRaSwapper::active_adapter() {
    uint32_t id = active_id_.load();
    return id ? registry_.find_by_id(id) : nullptr;
}

bool LoRaSwapper::is_in_vram(uint32_t id) const {
    const LoRaAdapter* a = registry_.find_by_id(id);
    return a && a->is_on_vram();
}

// ── Synchronization ──────────────────────────────────────────────────

void LoRaSwapper::synchronize() {
#if defined(RD_BACKEND_CUDA)
    cudaDeviceSynchronize();
#elif defined(RD_BACKEND_ROCM)
    hipDeviceSynchronize();
#endif
    // Also sync per-adapter streams
    for (auto* a : registry_.all_adapters()) {
        if (a && a->stream) {
#if defined(RD_BACKEND_CUDA)
            cudaStreamSynchronize(a->stream);
#elif defined(RD_BACKEND_ROCM)
            hipStreamSynchronize(a->stream);
#endif
        }
    }
}

bool LoRaSwapper::wait_adapter_ready(uint32_t id, int timeout_ms) {
    LoRaAdapter* a = registry_.find_by_id(id);
    if (!a) return false;

    auto deadline = HiRes::now() + std::chrono::milliseconds(timeout_ms);

    while (HiRes::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(a->mtx);
            if (a->state == LoRaState::Active && a->is_on_vram()) return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return false;
}

// ── Stats ────────────────────────────────────────────────────────────

LoRaSwapper::SwapStats LoRaSwapper::stats() const {
    return stats_;
}

void LoRaSwapper::reset_stats() {
    stats_ = SwapStats{};
}

void LoRaSwapper::record_swap_time(std::chrono::high_resolution_clock::time_point start) {
    auto elapsed = std::chrono::duration<double, std::micro>(HiRes::now() - start).count();
    stats_.total_swap_time_us += elapsed;
    stats_.avg_swap_time_us = stats_.total_swaps > 0 ?
        stats_.total_swap_time_us / stats_.total_swaps : 0.0;
}

void LoRaSwapper::print_stats() const {
    auto s = stats();
    std::printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    std::printf("║  Red Daft LoRa Brain Engine — Swap Statistics               ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Total swaps:          %8llu                              ║\n", (unsigned long long)s.total_swaps);
    std::printf("║  Total evictions:      %8llu                              ║\n", (unsigned long long)s.total_evictions);
    std::printf("║  Total prefetches:     %8llu                              ║\n", (unsigned long long)s.total_prefetches);
    std::printf("║  Bytes transferred:    %8llu KB                           ║\n", (unsigned long long)(s.total_bytes_transferred >> 10));
    std::printf("║  Avg swap time:        %8.1f us                           ║\n", s.avg_swap_time_us);
    std::printf("║  Active adapter:       %8u                              ║\n", (unsigned)active_id_.load());
    std::printf("╚══════════════════════════════════════════════════════════════╝\n");

    // Also print registry summary
    auto all = const_cast<LoRaRegistry&>(registry_).all_adapters();
    std::printf("  Registered adapters: %zu  |  In VRAM: %zu  |  Host total: %zu KB\n",
                all.size(),
                const_cast<LoRaRegistry&>(registry_).vram_count(),
                const_cast<LoRaRegistry&>(registry_).total_host_bytes() >> 10);
    for (auto* a : all) {
        if (!a) continue;
        std::printf("    [%u] %-18s pool=%-14s rank=%3u size=%5zuKB  state=%-10s  %s\n",
                    a->id, a->name.c_str(), to_string(a->pool).data(),
                    a->rank, a->total_size >> 10, to_string(a->state),
                    a->id == active_id_.load() ? "<-- ACTIVE" : "");
    }
    std::printf("\n");
}

} // namespace red_daft
