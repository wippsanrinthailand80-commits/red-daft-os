#pragma once
/**
 * red_daft_vram.h — Red Daft OS Custom VRAM Management Engine
 * =============================================================
 * High-performance, cross-platform tiered memory manager for LLM
 * inference/training on consumer GPUs. Minimizes VRAM by treating
 * System DDR4/DDR5 (pinned host memory) as a high-speed secondary
 * tier with async prefetching, dynamic offloading, and 10-pool
 * partitioning.
 *
 * Architecture:
 *   Tier 0: GPU VRAM          — active tensors, current layer, KV window
 *   Tier 1: Host DDR (pinned) — cold weights, long-context KV, overflow
 *   Async pipeline: cudaMemcpyAsync / hipMemcpyAsync on dedicated streams
 *                   with double-buffering to hide PCIe latency.
 *   Pools: 10 structured pools (see PoolType) sharing one elastic budget.
 *   Borrowing: Pool 9 (Emergency) lends to Pool 1/2 under pressure;
 *              otherwise LRU offload to host is triggered (anti-starvation).
 *
 * HAL: Macro-switchable at compile time:
 *   - Default (NVIDIA):  -DRD_USE_CUDA   (or auto-detect CUDA)
 *   - AMD ROCm:          -DRD_USE_ROCM   (-D__HIP_PLATFORM_AMD__)
 *   - CPU fallback:      no flag → malloc simulation (for CI/Kaggle CPU)
 *
 * Thread-safety: All public engine APIs are protected by per-pool
 * std::mutex + global mutex. C++20, -std=c++20.
 *
 * Author: Red Daft OS — Systems Performance Team
 * License: Red Daft OS Verbatim Distribution License v1.0 (see /LICENSE)
 *          — verbatim redistribution allowed (even commercially),
 *            private mods allowed but not distributable, no public forks
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <list>
#include <unordered_map>
#include <string>
#include <string_view>
#include <memory>
#include <atomic>
#include <chrono>
#include <optional>
#include <array>
#include <functional>

#if defined(RD_USE_ROCM) || defined(__HIP_PLATFORM_AMD__)
  #define RD_BACKEND_ROCM 1
  #include <hip/hip_runtime.h>
  #define RD_HAL_SUCCESS hipSuccess
#elif defined(RD_USE_CUDA) || defined(__CUDACC__) || __has_include(<cuda_runtime.h>)
  // Try CUDA if available, otherwise fall back to CPU simulation.
  #if __has_include(<cuda_runtime.h>)
    #define RD_BACKEND_CUDA 1
    #include <cuda_runtime.h>
    #define RD_HAL_SUCCESS cudaSuccess
  #else
    #define RD_BACKEND_CPU 1
  #endif
#else
  #define RD_BACKEND_CPU 1
#endif

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// HAL abstraction (host/device alloc, memcpy, streams)
// ─────────────────────────────────────────────────────────────────────
namespace hal {

#if defined(RD_BACKEND_ROCM)
  using Stream = hipStream_t;
  using Error  = hipError_t;
  inline const char* error_string(Error e) { return hipGetErrorString(e); }
#elif defined(RD_BACKEND_CUDA)
  using Stream = cudaStream_t;
  using Error  = cudaError_t;
  inline const char* error_string(Error e) { return cudaGetErrorString(e); }
#else // CPU fallback — simulate VRAM with malloc
  using Stream = void*;
  using Error  = int;
  constexpr Error kSuccess = 0;
  inline const char* error_string(Error) { return "CPU fallback (no GPU)"; }
#endif

struct HalConfig {
    int device_id = 0;
    bool enable_pinned_host = true;
    bool enable_async = true;
};

} // namespace hal

// ─────────────────────────────────────────────────────────────────────
// 10 Memory Pool Categories  (spec §1)
// ─────────────────────────────────────────────────────────────────────
enum class PoolType : uint8_t {
    ModelWeights          = 0, // Read-Only, static allocation (largest, LRU+freq)
    KvCache               = 1, // Dynamic paged expansion (arena, never auto-evict)
    ActivationsTensors    = 2, // High-frequency cyclic (FIFO/LRU)
    WorkspaceScratchpad   = 3, // Temporary kernel execution (FIFO ring)
    HostSwapStaging       = 4, // Pinned memory / host allocation staging
    EmbeddingBuffers      = 5, // Embedding tables (LRU + prefetch)
    QuantizationMetadata  = 6, // Scales, zero-points, codebooks
    AsyncStreamQueue      = 7, // Stream descriptors, double-buffer control blocks
    SystemIpcShared       = 8, // IPC / shared memory handles
    EmergencyOverflow     = 9, // Dynamic borrowing pool (lends to 1/2 under pressure)
    COUNT                 = 10
};
constexpr size_t kPoolCount = 10;
inline constexpr std::array<std::string_view, kPoolCount> kPoolNames = {
    "ModelWeights", "KvCache", "ActivationsTensors", "WorkspaceScratchpad",
    "HostSwapStaging", "EmbeddingBuffers", "QuantizationMetadata",
    "AsyncStreamQueue", "SystemIpcShared", "EmergencyOverflow"
};
inline std::string_view to_string(PoolType p) {
    return kPoolNames[static_cast<size_t>(p)];
}
// Policy hint per pool (used by eviction engine)
enum class EvictionPolicy : uint8_t {
    LRU_FREQ,   // LRU with frequency tie-break (weights, embeddings)
    FIFO,       // Ring / queue (activations, workspace)
    ARENA,      // Never auto-evict; explicit free only (KV)
    LRU_GENERIC // Plain LRU (others)
};
inline EvictionPolicy default_policy(PoolType p) {
    switch (p) {
        case PoolType::ModelWeights:       return EvictionPolicy::LRU_FREQ;
        case PoolType::EmbeddingBuffers:   return EvictionPolicy::LRU_FREQ;
        case PoolType::KvCache:            return EvictionPolicy::ARENA;
        case PoolType::ActivationsTensors: return EvictionPolicy::FIFO;
        case PoolType::WorkspaceScratchpad:return EvictionPolicy::FIFO;
        default:                           return EvictionPolicy::LRU_GENERIC;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Core data structures
// ─────────────────────────────────────────────────────────────────────
using Clock = std::chrono::steady_clock;

struct MemoryBlock {
    uint64_t    id = 0;                 // unique handle (returned to caller)
    void*       vram_ptr = nullptr;     // device pointer (cudaMalloc/hipMalloc)
    void*       host_ptr = nullptr;     // pinned host pointer (cudaMallocHost)
    void*       shadow_ptr = nullptr;   // optional second buffer for double-buffering
    size_t      size = 0;               // requested size
    size_t      aligned_size = 0;       // aligned to 256B
    PoolType    pool = PoolType::ModelWeights;
    int         device_id = 0;
    bool        is_on_vram = false;     // currently resident in VRAM?
    bool        is_on_host = false;     // has valid host copy?
    bool        is_pinned = false;      // host_ptr is cudaHostAlloc?
    bool        is_dirty = false;       // VRAM newer than host (needs writeback)?
    bool        is_borrowed = false;    // came from Emergency pool?
    PoolType    borrowed_from = PoolType::EmergencyOverflow;
    Clock::time_point last_access = Clock::now();
    uint32_t    access_freq = 1;        // for LRU_FREQ
    hal::Stream stream = nullptr;       // associated async stream (if any)

    // Debug / stats
    std::string tag;                    // optional user tag e.g. "layer_12_qkv"
};

struct TieredBuffer {
    // Double-buffered view over a MemoryBlock. Front = active VRAM buffer,
    // back = host shadow used for prefetch. Swap is async on stream.
    MemoryBlock* block = nullptr;
    void* front() const { return block ? block->vram_ptr : nullptr; }
    void* back()  const { return block ? block->host_ptr : nullptr; }
    bool double_buffered() const { return block && block->shadow_ptr != nullptr; }
};

struct PoolStats {
    PoolType pool;
    size_t quota_bytes = 0;      // budget for this pool (VRAM tier)
    size_t used_vram = 0;        // currently resident in VRAM
    size_t used_host = 0;        // pinned host bytes
    size_t borrowed_bytes = 0;   // borrowed from Emergency (or lent out)
    size_t block_count = 0;
    size_t evictions = 0;
    size_t hits = 0;
    size_t misses = 0;
    size_t prefetch_issued = 0;
    size_t offload_issued = 0;
    size_t oom_count = 0;
};

struct EngineConfig {
    int    device_id = 0;
    size_t vram_budget_bytes = 0;   // 0 = auto (80% of free VRAM, or 2 GiB fallback)
    size_t host_budget_bytes = 0;   // 0 = auto (50% of system, or 8 GiB fallback)
    size_t emergency_reserve_bytes = 256ULL << 20; // 256 MiB reserved for Pool 9
    double high_pressure_threshold = 0.85; // 85% quota → trigger borrow/offload
    double low_pressure_threshold  = 0.60; // hysteresis for anti-starvation
    bool   enable_double_buffer = true;
    bool   enable_prefetch = true;
    size_t alignment = 256;
    int    num_streams = 4;        // dedicated async streams
};

// Lightweight handle returned to Python / callers (avoids raw pointers)
using BlockHandle = uint64_t;
constexpr BlockHandle kInvalidHandle = 0;

// ─────────────────────────────────────────────────────────────────────
// MemoryPool — one of the 10 pools, thread-safe
// ─────────────────────────────────────────────────────────────────────
class MemoryPool {
public:
    explicit MemoryPool(PoolType type, size_t quota_bytes, EvictionPolicy policy);
    ~MemoryPool();

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;

    // Core ops (internal, engine holds pool mutex externally for batch)
    PoolType type() const { return type_; }
    EvictionPolicy policy() const { return policy_; }
    size_t quota() const { return quota_bytes_; }
    void set_quota(size_t q) { quota_bytes_ = q; }

    size_t used_vram() const { return used_vram_; }
    size_t used_host() const { return used_host_; }
    size_t block_count() const { return blocks_.size(); }
    bool has_space(size_t bytes) const { return used_vram_ + bytes <= quota_bytes_; }
    double pressure() const; // used / quota [0,1]

    // LRU / FIFO ordering: front = MRU, back = LRU (victim)
    void touch(MemoryBlock* blk);   // move to front, bump freq
    MemoryBlock* lru_victim();      // back element (or nullptr)

    // Bookkeeping
    void track_block(std::unique_ptr<MemoryBlock> blk);
    std::unique_ptr<MemoryBlock> untrack_block(BlockHandle id);
    MemoryBlock* find_block(BlockHandle id);

    PoolStats snapshot() const;
    std::mutex& mutex() { return mutex_; }
    std::mutex& mutex() const { return mutex_; }

private:
    PoolType type_;
    size_t quota_bytes_;
    EvictionPolicy policy_;
    size_t used_vram_ = 0;
    size_t used_host_ = 0;
    size_t borrowed_bytes_ = 0;

    std::list<MemoryBlock*> lru_; // MRU front, LRU back
    std::unordered_map<BlockHandle, std::unique_ptr<MemoryBlock>> blocks_;
    mutable std::mutex mutex_;

    // stats
    size_t evictions_ = 0, hits_ = 0, misses_ = 0;
    size_t prefetch_issued_ = 0, offload_issued_ = 0, oom_ = 0;
    friend class VramEngine;
};

// ─────────────────────────────────────────────────────────────────────
// VramEngine — top-level manager, singleton, owns 10 pools + HAL streams
// ─────────────────────────────────────────────────────────────────────
class VramEngine {
public:
    static VramEngine& instance();

    VramEngine(const VramEngine&) = delete;
    VramEngine& operator=(const VramEngine&) = delete;

    // Lifecycle
    bool initialize(const EngineConfig& cfg = {});
    void shutdown();
    bool is_initialized() const { return initialized_; }
    const EngineConfig& config() const { return config_; }

    // ── Core API (spec § Required Deliverables) ──────────────────────
    // allocate: best-effort VRAM allocation in pool; may borrow or offload.
    // Returns handle (block id) or kInvalidHandle on OOM. Thread-safe.
    BlockHandle allocate(PoolType pool, size_t size, size_t alignment = 0,
                         std::string_view tag = {});

    // deallocate: frees both tiers and returns quota. Thread-safe.
    bool deallocate(BlockHandle handle);

    // prefetch_to_vram: async host→device copy on dedicated stream.
    // If block already on VRAM, just touches LRU and returns true.
    // double_buffer: if true, allocates shadow and swaps on stream.
    bool prefetch_to_vram(BlockHandle handle, hal::Stream stream = nullptr,
                          bool double_buffer = true);

    // offload_to_ddr: async device→host copy, optionally keep VRAM shadow.
    // keep_vram_copy=false frees VRAM after copy (capacity tier).
    bool offload_to_ddr(BlockHandle handle, hal::Stream stream = nullptr,
                        bool keep_vram_copy = false);

    // borrow_memory: dynamic borrowing engine (spec §4).
    // If `requesting_pool` (K/V or Activations) is under pressure, try to
    // lend `bytes` from Emergency pool. Returns true if borrowed.
    // Anti-starvation: if Emergency also pressured, triggers LRU offload
    // of cold blocks to host before lending.
    bool borrow_memory(PoolType requesting_pool, size_t bytes);

    // Return borrowed quota (called internally on deallocate or explicitly)
    bool return_borrowed(PoolType pool, size_t bytes);

    // Introspection
    PoolStats get_pool_stats(PoolType pool) const;
    std::array<PoolStats, kPoolCount> get_all_stats() const;
    void print_pool_stats() const; // pretty print to stdout

    // Low-level HAL helpers (exposed for advanced use)
    hal::Stream get_stream(size_t idx = 0) const;
    hal::Stream get_next_stream(); // round-robin
    void synchronize_all_streams() const;
    void* raw_device_ptr(BlockHandle handle) const;
    void* raw_host_ptr(BlockHandle handle) const;

    // Utility: try to free `bytes` in pool by offloading LRU victims to host
    size_t offload_lru_to_host(PoolType pool, size_t bytes_needed);

private:
    VramEngine() = default;
    ~VramEngine() { shutdown(); }

    // internal helpers
    static size_t align_up(size_t n, size_t alignment);
    MemoryPool& pool_of(PoolType p) { return *pools_[static_cast<size_t>(p)]; }
    const MemoryPool& pool_of(PoolType p) const { return *pools_[static_cast<size_t>(p)]; }

    bool ensure_capacity(PoolType pool, size_t bytes);
    bool try_borrow_from_emergency(PoolType requester, size_t bytes);
    void record_access(MemoryBlock* blk);

    // HAL wrappers (member helpers that call the global HAL)
    bool hal_malloc_device(void** ptr, size_t size);
    bool hal_malloc_host(void** ptr, size_t size, bool pinned = true);
    void hal_free_device(void* ptr);
    void hal_free_host(void* ptr, bool pinned);
    bool hal_memcpy_async(void* dst, const void* src, size_t bytes,
                          hal::Stream stream, bool host_to_device);

    EngineConfig config_{};
    std::array<std::unique_ptr<MemoryPool>, kPoolCount> pools_;
    std::vector<hal::Stream> streams_;
    std::atomic<size_t> next_stream_idx_{0};
    std::atomic<uint64_t> next_block_id_{1};
    std::atomic<bool> initialized_{false};
    mutable std::shared_mutex global_mutex_; // protects pools_ creation + cross-pool borrow
    // block handle → pool index for O(1) lookup
    std::unordered_map<BlockHandle, PoolType> handle_to_pool_;
    mutable std::mutex handle_map_mutex_;
};

// ── Free-function wrappers (C-style convenience) ──────────────────────
inline bool initialize(const EngineConfig& cfg = {}) {
    return VramEngine::instance().initialize(cfg);
}
inline BlockHandle allocate(PoolType pool, size_t size,
                            size_t alignment = 0, std::string_view tag = {}) {
    return VramEngine::instance().allocate(pool, size, alignment, tag);
}
inline bool deallocate(BlockHandle h) {
    return VramEngine::instance().deallocate(h);
}
inline bool prefetch_to_vram(BlockHandle h, hal::Stream s = nullptr, bool db = true) {
    return VramEngine::instance().prefetch_to_vram(h, s, db);
}
inline bool offload_to_ddr(BlockHandle h, hal::Stream s = nullptr, bool keep = false) {
    return VramEngine::instance().offload_to_ddr(h, s, keep);
}
inline bool borrow_memory(PoolType p, size_t b) {
    return VramEngine::instance().borrow_memory(p, b);
}
inline void print_pool_stats() {
    VramEngine::instance().print_pool_stats();
}

} // namespace red_daft
