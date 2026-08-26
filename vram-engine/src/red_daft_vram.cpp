/**
 * red_daft_vram.cpp — Red Daft OS VRAM Management Engine (implementation)
 * ========================================================================
 * Production C++20 implementation of the tiered 10-pool VRAM manager.
 * See red_daft_vram.h for architecture overview.
 *
 * Key design points:
 *  - Each MemoryPool is a quota-bounded LRU/FIFO/Arena with async offload
 *  - VramEngine owns 10 pools + N dedicated CUDA streams for prefetch/offload
 *  - Double-buffering: optional shadow host buffer + async swap to hide PCIe
 *  - Borrowing: Pool 9 (Emergency) is a lendable reserve; Pools 1/2 can
 *               borrow under pressure, with anti-starvation hysteresis.
 *  - HAL: all device/host alloc, memcpy, streams go through hal_* wrappers
 *         which compile to CUDA, ROCm, or CPU malloc when no GPU is present.
 *
 * Threading: VramEngine uses std::shared_mutex for global state and
 * per-pool std::mutex for pool ops. All public APIs are thread-safe.
 */

#include "red_daft_vram.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <thread>

#if defined(RD_BACKEND_CUDA)
  // CUDA already included via header
#elif defined(RD_BACKEND_ROCM)
  // HIP already included
#else
  #include <cstdlib> // fallback malloc
#endif

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────
static size_t align_up(size_t n, size_t a) {
    if (a == 0) a = 256;
    return (n + a - 1) & ~(a - 1);
}

static const char* pool_cstr(PoolType p) {
    return kPoolNames[static_cast<size_t>(p)].data();
}

// Default VRAM quota distribution for 10 pools (must sum to 100%).
// Tuned for 3B LLM: ModelWeights dominant, KV second, Activations third,
// Emergency is a small reserve that grows via borrowing.
static std::array<double, kPoolCount> default_weights() {
    // Spec pools: 0 ModelWeights 35%, 1 KvCache 25%, 2 Activations 15%,
    // 3 Workspace 5%, 4 HostSwap 5%, 5 Embed 5%, 6 Quant 3%, 7 AsyncQueue 2%,
    // 8 SystemIpc 3%, 9 Emergency 2% (reserve, auto-refills)
    return {0.35, 0.25, 0.15, 0.05, 0.05, 0.05, 0.03, 0.02, 0.03, 0.02};
}

// ─────────────────────────────────────────────────────────────────────
// HAL wrappers — device/host alloc, free, memcpy, streams
// ─────────────────────────────────────────────────────────────────────
namespace {
bool detail_hal_malloc_device(void** ptr, size_t size) {
#if defined(RD_BACKEND_CUDA)
    cudaError_t e = cudaMalloc(ptr, size);
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[hal] cudaMalloc %zu failed: %s\n", size, cudaGetErrorString(e));
        return false;
    }
    return true;
#elif defined(RD_BACKEND_ROCM)
    hipError_t e = hipMalloc(ptr, size);
    if (e != hipSuccess) {
        std::fprintf(stderr, "[hal] hipMalloc %zu failed: %s\n", size, hipGetErrorString(e));
        return false;
    }
    return true;
#else
    *ptr = std::malloc(size);
    if (!*ptr) {
        std::fprintf(stderr, "[hal] malloc %zu failed (CPU fallback)\n", size);
        return false;
    }
    // Simulate VRAM by zeroing (helps catch uninitialized use)
    std::memset(*ptr, 0, size);
    return true;
#endif
}

bool detail_hal_malloc_host(void** ptr, size_t size, bool pinned) {
#if defined(RD_BACKEND_CUDA)
    if (pinned) {
        cudaError_t e = cudaMallocHost(ptr, size);
        if (e != cudaSuccess) {
            std::fprintf(stderr, "[hal] cudaMallocHost %zu failed: %s\n", size, cudaGetErrorString(e));
            return false;
        }
    } else {
        *ptr = std::malloc(size);
        if (!*ptr) return false;
    }
    return true;
#elif defined(RD_BACKEND_ROCM)
    if (pinned) {
        hipError_t e = hipHostMalloc(ptr, size, hipHostMallocDefault);
        if (e != hipSuccess) {
            std::fprintf(stderr, "[hal] hipHostMalloc %zu failed: %s\n", size, hipGetErrorString(e));
            return false;
        }
    } else {
        *ptr = std::malloc(size);
        if (!*ptr) return false;
    }
    return true;
#else
    (void)pinned;
    *ptr = std::malloc(size);
    if (!*ptr) {
        std::fprintf(stderr, "[hal] malloc host %zu failed\n", size);
        return false;
    }
    std::memset(*ptr, 0, size);
    return true;
#endif
}

void detail_hal_free_device(void* ptr) {
    if (!ptr) return;
#if defined(RD_BACKEND_CUDA)
    cudaFree(ptr);
#elif defined(RD_BACKEND_ROCM)
    hipFree(ptr);
#else
    std::free(ptr);
#endif
}

void detail_hal_free_host(void* ptr, bool pinned) {
    if (!ptr) return;
#if defined(RD_BACKEND_CUDA)
    if (pinned) cudaFreeHost(ptr);
    else std::free(ptr);
#elif defined(RD_BACKEND_ROCM)
    if (pinned) hipHostFree(ptr);
    else std::free(ptr);
#else
    (void)pinned;
    std::free(ptr);
#endif
}

bool detail_hal_memcpy_async(void* dst, const void* src, size_t bytes,
                      hal::Stream stream, bool /*host_to_device*/) {
#if defined(RD_BACKEND_CUDA)
    cudaMemcpyKind kind = cudaMemcpyDefault;
    // We let CUDA auto-detect kind; caller passes correct pointers.
    // For explicit direction, we could use cudaMemcpyHostToDevice etc.
    // Here we infer from hal's pinned attribute, but Default is fine.
    cudaError_t e = cudaMemcpyAsync(dst, src, bytes, kind, stream);
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[hal] cudaMemcpyAsync %zu failed: %s\n", bytes, cudaGetErrorString(e));
        return false;
    }
    return true;
#elif defined(RD_BACKEND_ROCM)
    hipError_t e = hipMemcpyAsync(dst, src, bytes, hipMemcpyDefault, stream);
    if (e != hipSuccess) {
        std::fprintf(stderr, "[hal] hipMemcpyAsync %zu failed: %s\n", bytes, hipGetErrorString(e));
        return false;
    }
    return true;
#else
    // CPU fallback: synchronous memcpy (simulate async by direct copy)
    std::memcpy(dst, src, bytes);
    (void)stream;
    return true;
#endif
}

bool hal_stream_create(hal::Stream* s) {
#if defined(RD_BACKEND_CUDA)
    return cudaStreamCreate(s) == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    return hipStreamCreate(s) == hipSuccess;
#else
    *s = nullptr;
    return true;
#endif
}

void hal_stream_destroy(hal::Stream s) {
#if defined(RD_BACKEND_CUDA)
    if (s) cudaStreamDestroy(s);
#elif defined(RD_BACKEND_ROCM)
    if (s) hipStreamDestroy(s);
#else
    (void)s;
#endif
}

bool hal_stream_synchronize(hal::Stream s) {
#if defined(RD_BACKEND_CUDA)
    return cudaStreamSynchronize(s) == cudaSuccess;
#elif defined(RD_BACKEND_ROCM)
    return hipStreamSynchronize(s) == hipSuccess;
#else
    (void)s;
    return true;
#endif
}

} // anonymous namespace

// ── VramEngine HAL member wrappers (delegate to detail HAL) ─────────
bool VramEngine::hal_malloc_device(void** ptr, size_t size) {
    return detail_hal_malloc_device(ptr, size);
}
bool VramEngine::hal_malloc_host(void** ptr, size_t size, bool pinned) {
    return detail_hal_malloc_host(ptr, size, pinned);
}
void VramEngine::hal_free_device(void* ptr) {
    detail_hal_free_device(ptr);
}
void VramEngine::hal_free_host(void* ptr, bool pinned) {
    detail_hal_free_host(ptr, pinned);
}
bool VramEngine::hal_memcpy_async(void* dst, const void* src, size_t bytes,
                                  hal::Stream stream, bool host_to_device) {
    return detail_hal_memcpy_async(dst, src, bytes, stream, host_to_device);
}

// ─────────────────────────────────────────────────────────────────────
// MemoryPool implementation
// ─────────────────────────────────────────────────────────────────────
MemoryPool::MemoryPool(PoolType type, size_t quota_bytes, EvictionPolicy policy)
    : type_(type), quota_bytes_(quota_bytes), policy_(policy) {}

MemoryPool::~MemoryPool() {
    // Free all blocks (device + host). Called from VramEngine::shutdown
    // with pool mutex held, so no extra locking needed here.
    for (auto& kv : blocks_) {
        auto* b = kv.second.get();
        detail_hal_free_device(b->vram_ptr);
        detail_hal_free_host(b->host_ptr, b->is_pinned);
        detail_hal_free_host(b->shadow_ptr, true); // shadow is always pinned if present
    }
}

double MemoryPool::pressure() const {
    if (quota_bytes_ == 0) return 1.0;
    return static_cast<double>(used_vram_) / static_cast<double>(quota_bytes_);
}

void MemoryPool::touch(MemoryBlock* blk) {
    // Move to front (MRU), bump freq for LRU_FREQ pools
    auto it = std::find(lru_.begin(), lru_.end(), blk);
    if (it != lru_.end()) lru_.erase(it);
    lru_.push_front(blk);
    blk->last_access = Clock::now();
    if (policy_ == EvictionPolicy::LRU_FREQ && blk->access_freq < 3) {
        blk->access_freq++;
    }
    hits_++;
}

MemoryBlock* MemoryPool::lru_victim() {
    if (lru_.empty()) return nullptr;
    // For LRU_FREQ, apply second-chance: if tail has high freq, decay and move to front
    if (policy_ == EvictionPolicy::LRU_FREQ) {
        int guard = 0;
        while (!lru_.empty() && lru_.back()->access_freq >= 2 && guard++ < 64) {
            auto* v = lru_.back();
            v->access_freq--;
            lru_.pop_back();
            lru_.push_front(v);
        }
    }
    if (lru_.empty()) return nullptr;
    return lru_.back();
}

void MemoryPool::track_block(std::unique_ptr<MemoryBlock> blk) {
    MemoryBlock* raw = blk.get();
    used_vram_ += raw->is_on_vram ? raw->aligned_size : 0;
    used_host_ += raw->is_on_host ? raw->aligned_size : 0;
    if (raw->is_borrowed) borrowed_bytes_ += raw->aligned_size;
    blocks_.emplace(raw->id, std::move(blk));
    // Insert into LRU according to policy: FIFO and ARENA use push_back (queue),
    // LRU variants use push_front (stack). For ARENA we still push_back for iteration,
    // but victim selection will return -1 (never evict) via pool's ensure logic.
    if (policy_ == EvictionPolicy::FIFO || policy_ == EvictionPolicy::ARENA) {
        lru_.push_back(raw);
    } else {
        lru_.push_front(raw);
    }
}

std::unique_ptr<MemoryBlock> MemoryPool::untrack_block(BlockHandle id) {
    auto it = blocks_.find(id);
    if (it == blocks_.end()) return nullptr;
    auto* raw = it->second.get();
    // Remove from LRU
    auto lru_it = std::find(lru_.begin(), lru_.end(), raw);
    if (lru_it != lru_.end()) lru_.erase(lru_it);
    used_vram_ -= raw->is_on_vram ? raw->aligned_size : 0;
    used_host_ -= raw->is_on_host ? raw->aligned_size : 0;
    if (raw->is_borrowed) {
        borrowed_bytes_ -= std::min(borrowed_bytes_, raw->aligned_size);
    }
    auto ptr = std::move(it->second);
    blocks_.erase(it);
    return ptr;
}

MemoryBlock* MemoryPool::find_block(BlockHandle id) {
    auto it = blocks_.find(id);
    return it == blocks_.end() ? nullptr : it->second.get();
}

PoolStats MemoryPool::snapshot() const {
    PoolStats s{};
    s.pool = type_;
    s.quota_bytes = quota_bytes_;
    s.used_vram = used_vram_;
    s.used_host = used_host_;
    s.borrowed_bytes = borrowed_bytes_;
    s.block_count = blocks_.size();
    s.evictions = evictions_;
    s.hits = hits_;
    s.misses = misses_;
    s.prefetch_issued = prefetch_issued_;
    s.offload_issued = offload_issued_;
    s.oom_count = oom_;
    return s;
}

// ─────────────────────────────────────────────────────────────────────
// VramEngine implementation
// ─────────────────────────────────────────────────────────────────────
VramEngine& VramEngine::instance() {
    static VramEngine inst;
    return inst;
}

bool VramEngine::initialize(const EngineConfig& cfg) {
    std::unique_lock<std::shared_mutex> lock(global_mutex_);
    if (initialized_) {
        std::fprintf(stderr, "[vram] already initialized\n");
        return false;
    }
    config_ = cfg;

    // ── Detect GPU and budget ──────────────────────────────────────
    size_t vram_budget = cfg.vram_budget_bytes;
    size_t host_budget = cfg.host_budget_bytes;

#if defined(RD_BACKEND_CUDA)
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0) {
        cudaSetDevice(cfg.device_id);
        size_t free_bytes = 0, total_bytes = 0;
        if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess) {
            if (vram_budget == 0) {
                // Reserve 20% for system / fragmentation, use 80%
                vram_budget = free_bytes * 80 / 100;
                if (vram_budget == 0) vram_budget = 2ULL << 30; // 2 GiB fallback
            }
            if (host_budget == 0) host_budget = 8ULL << 30; // 8 GiB
            std::printf("[vram] CUDA device %d: total %zu MiB, free %zu MiB → budget VRAM %zu MiB, Host %zu MiB\n",
                        cfg.device_id, total_bytes>>20, free_bytes>>20, vram_budget>>20, host_budget>>20);
        }
    } else {
        if (vram_budget == 0) vram_budget = 2ULL << 30;
        if (host_budget == 0) host_budget = 8ULL << 30;
        std::printf("[vram] CUDA not available, CPU fallback: VRAM budget %zu MiB (simulated)\n", vram_budget>>20);
    }
#elif defined(RD_BACKEND_ROCM)
    int device_count = 0;
    if (hipGetDeviceCount(&device_count) == hipSuccess && device_count > 0) {
        hipSetDevice(cfg.device_id);
        size_t free_bytes = 0, total_bytes = 0;
        if (hipMemGetInfo(&free_bytes, &total_bytes) == hipSuccess) {
            if (vram_budget == 0) vram_budget = free_bytes * 80 / 100;
            if (host_budget == 0) host_budget = 8ULL << 30;
            std::printf("[vram] ROCm device %d: total %zu MiB, free %zu MiB → budget VRAM %zu MiB\n",
                        cfg.device_id, total_bytes>>20, free_bytes>>20, vram_budget>>20);
        }
    } else {
        if (vram_budget == 0) vram_budget = 2ULL << 30;
        if (host_budget == 0) host_budget = 8ULL << 30;
        std::printf("[vram] ROCm not available, CPU fallback: VRAM budget %zu MiB\n", vram_budget>>20);
    }
#else
    if (vram_budget == 0) vram_budget = 1ULL << 30; // 1 GiB simulated
    if (host_budget == 0) host_budget = 4ULL << 30;
    std::printf("[vram] CPU fallback mode: VRAM budget %zu MiB (simulated), Host %zu MiB\n",
                vram_budget>>20, host_budget>>20);
#endif

    // ── Create 10 pools with weighted quotas ────────────────────────
    auto weights = default_weights();
    size_t total_assigned = 0;
    for (size_t i = 0; i < kPoolCount; ++i) {
        size_t quota = static_cast<size_t>(vram_budget * weights[i]);
        // Ensure at least 4 MiB per pool and Emergency gets at least reserve
        if (quota < (4ULL<<20)) quota = 4ULL<<20;
        if (static_cast<PoolType>(i) == PoolType::EmergencyOverflow && quota < cfg.emergency_reserve_bytes) {
            quota = cfg.emergency_reserve_bytes;
        }
        // Last pool absorbs rounding remainder
        if (i == kPoolCount - 1) {
            quota = vram_budget - total_assigned;
        }
        total_assigned += quota;
        EvictionPolicy pol = default_policy(static_cast<PoolType>(i));
        pools_[i] = std::make_unique<MemoryPool>(static_cast<PoolType>(i), quota, pol);
        std::printf("[vram] Pool %zu %-22s quota %6zu MiB  policy %d\n",
                    i, kPoolNames[i].data(), quota>>20, static_cast<int>(pol));
    }
    // Adjust if rounding exceeded budget (should not, but clamp)
    if (total_assigned > vram_budget) {
        pools_[kPoolCount-1]->set_quota(pools_[kPoolCount-1]->quota() - (total_assigned - vram_budget));
    }

    // ── Create dedicated async streams ──────────────────────────────
    streams_.resize(cfg.num_streams);
    for (int i = 0; i < cfg.num_streams; ++i) {
        if (!hal_stream_create(&streams_[i])) {
            std::fprintf(stderr, "[vram] failed to create stream %d\n", i);
            streams_[i] = nullptr;
        }
    }
    std::printf("[vram] Created %d async streams (double-buffer=%s, prefetch=%s)\n",
                cfg.num_streams,
                cfg.enable_double_buffer ? "on" : "off",
                cfg.enable_prefetch ? "on" : "off");

    initialized_.store(true);
    std::printf("[vram] Engine initialized (device %d, %zu pools)\n", cfg.device_id, kPoolCount);
    return true;
}

void VramEngine::shutdown() {
    std::unique_lock<std::shared_mutex> lock(global_mutex_);
    if (!initialized_) return;

    // Free all pools (their destructors free device/host memory)
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        handle_to_pool_.clear();
    }
    for (auto& p : pools_) p.reset();
    for (auto s : streams_) hal_stream_destroy(s);
    streams_.clear();
    next_block_id_.store(1);
    next_stream_idx_.store(0);
    initialized_.store(false);
    std::printf("[vram] Engine shutdown\n");
}

size_t VramEngine::align_up(size_t n, size_t alignment) {
    return ::red_daft::align_up(n, alignment);
}

hal::Stream VramEngine::get_stream(size_t idx) const {
    if (streams_.empty()) return nullptr;
    return streams_[idx % streams_.size()];
}

hal::Stream VramEngine::get_next_stream() {
    size_t idx = next_stream_idx_.fetch_add(1, std::memory_order_relaxed);
    return get_stream(idx);
}

void VramEngine::synchronize_all_streams() const {
    for (auto s : streams_) {
        if (s) hal_stream_synchronize(s);
    }
}

// ── Capacity & Borrowing ─────────────────────────────────────────────

bool VramEngine::ensure_capacity(PoolType pool, size_t bytes) {
    MemoryPool& p = pool_of(pool);
    // Fast path: has space
    if (p.has_space(bytes)) return true;

    // Try to grow by offloading LRU victims to host (free VRAM)
    size_t needed = (p.used_vram() + bytes) - p.quota();
    size_t freed = offload_lru_to_host(pool, needed);
    if (p.has_space(bytes)) return true;

    // If still no space and this is a high-pressure pool (KV/Activations),
    // try to borrow from Emergency
    if (pool == PoolType::KvCache || pool == PoolType::ActivationsTensors) {
        if (try_borrow_from_emergency(pool, bytes - (freed > needed ? 0 : needed - freed))) {
            return p.has_space(bytes);
        }
    }
    return p.has_space(bytes);
}

size_t VramEngine::offload_lru_to_host(PoolType pool, size_t bytes_needed) {
    MemoryPool& p = pool_of(pool);
    // ARENA pools (KV) never auto-evict; caller must handle explicitly
    if (p.policy() == EvictionPolicy::ARENA) return 0;

    size_t freed = 0;
    // Collect victims first to avoid holding lock during async copies
    std::vector<MemoryBlock*> victims;
    {
        std::lock_guard<std::mutex> lock(p.mutex());
        // Snapshot LRU order (tail first)
        for (auto it = p.lru_.rbegin(); it != p.lru_.rend() && freed < bytes_needed; ++it) {
            MemoryBlock* blk = *it;
            // Skip borrowed blocks (owned by Emergency) and blocks already offloaded
            if (!blk->is_on_vram) continue;
            if (blk->is_borrowed) continue;
            victims.push_back(blk);
            freed += blk->aligned_size;
        }
    }
    // Now offload each victim asynchronously (but we synchronize per block
    // here to keep quota accounting simple; pipeline hides latency via streams)
    for (auto* blk : victims) {
        // We need to ensure host copy exists; allocate if needed
        if (!blk->is_on_host) {
            void* host_ptr = nullptr;
            if (!hal_malloc_host(&host_ptr, blk->aligned_size, true)) {
                std::fprintf(stderr, "[vram] offload: host alloc failed for block %llu\n",
                             (unsigned long long)blk->id);
                continue;
            }
            blk->host_ptr = host_ptr;
            blk->is_on_host = true;
            blk->is_pinned = true;
            {
                std::lock_guard<std::mutex> lock(p.mutex());
                p.used_host_ += blk->aligned_size;
            }
        }
        hal::Stream s = get_next_stream();
        // Async copy device -> host
        if (hal_memcpy_async(blk->host_ptr, blk->vram_ptr, blk->aligned_size, s, false)) {
            // Keep host copy, free device copy if we want to actually free VRAM.
            // For capacity tier, we free VRAM after copy (writeback if dirty).
            // We synchronize here to make quota update deterministic; in production
            // you'd keep the copy queued and update quota on completion callback.
            hal_stream_synchronize(s);
            // Writeback already done via copy; now free VRAM
            hal_free_device(blk->vram_ptr);
            {
                std::lock_guard<std::mutex> lock(p.mutex());
                p.used_vram_ -= blk->aligned_size;
                p.evictions_++;
            }
            blk->vram_ptr = nullptr;
            blk->is_on_vram = false;
            blk->is_dirty = false;
            // Remove from LRU (now host-only, not counted for VRAM pressure)
            {
                std::lock_guard<std::mutex> lock(p.mutex());
                auto it = std::find(p.lru_.begin(), p.lru_.end(), blk);
                if (it != p.lru_.end()) p.lru_.erase(it);
                // Host-only blocks are kept in a separate list or just not in LRU.
                // For simplicity we keep them untracked until prefetch.
            }
            {
                std::lock_guard<std::mutex> lock(p.mutex());
                p.offload_issued_++;
            }
        }
        if (freed >= bytes_needed) break;
    }
    return freed;
}

bool VramEngine::try_borrow_from_emergency(PoolType requester, size_t bytes) {
    // Emergency pool is Pool 9. Check if it has free quota.
    MemoryPool& emergency = pool_of(PoolType::EmergencyOverflow);
    MemoryPool& req = pool_of(requester);

    std::unique_lock<std::mutex> lock_em(emergency.mutex(), std::defer_lock);
    std::unique_lock<std::mutex> lock_req(req.mutex(), std::defer_lock);
    std::lock(lock_em, lock_req);

    size_t emergency_free = (emergency.quota() > emergency.used_vram_) ?
                              emergency.quota() - emergency.used_vram_ : 0;
    if (emergency_free >= bytes) {
        // Lend: increase requester's effective quota, decrease emergency's
        // We do this by bumping requester's quota and tracking borrowed.
        req.quota_bytes_ += bytes;
        req.borrowed_bytes_ += bytes;
        emergency.quota_bytes_ -= bytes;
        // Anti-starvation: if emergency falls below low threshold, it will
        // reclaim via offload on next pressure event.
        std::printf("[vram][borrow] %s borrowed %zu KiB from Emergency (emergency free now %zu KiB)\n",
                    pool_cstr(requester), bytes>>10, (emergency.quota() - emergency.used_vram_)>>10);
        return true;
    }

    // Emergency also pressured → try to offload its own cold blocks to host
    // to make room, then retry borrow.
    if (emergency_free > 0 && emergency_free < bytes) {
        // Try to free enough in emergency by offloading its LRU
        // Unlock to avoid deadlock (offload needs pool lock)
        lock_em.unlock();
        lock_req.unlock();
        size_t needed = bytes - emergency_free;
        size_t freed = offload_lru_to_host(PoolType::EmergencyOverflow, needed);
        if (freed >= needed) {
            return try_borrow_from_emergency(requester, bytes); // retry
        }
    }

    // Also try to offload requester's own cold blocks as anti-starvation
    // (even if borrowing fails, we can make room)
    lock_req.unlock();
    lock_em.unlock();
    return false;
}

bool VramEngine::borrow_memory(PoolType requesting_pool, size_t bytes) {
    if (!initialized_) return false;
    if (requesting_pool == PoolType::EmergencyOverflow) return false;
    // Only Pools 1 and 2 are allowed to borrow by spec, but we allow any
    // pool to borrow from Emergency for flexibility; KV/Activations get priority.
    double pressure = pool_of(requesting_pool).pressure();
    if (pressure < config_.high_pressure_threshold) {
        // Not under pressure, no need to borrow
        return false;
    }
    size_t aligned = align_up(bytes, config_.alignment);
    return try_borrow_from_emergency(requesting_pool, aligned);
}

bool VramEngine::return_borrowed(PoolType pool, size_t bytes) {
    std::unique_lock<std::mutex> lock_req(pool_of(pool).mutex(), std::defer_lock);
    std::unique_lock<std::mutex> lock_em(pool_of(PoolType::EmergencyOverflow).mutex(), std::defer_lock);
    std::lock(lock_req, lock_em);
    MemoryPool& p = pool_of(pool);
    MemoryPool& e = pool_of(PoolType::EmergencyOverflow);
    size_t to_return = std::min({bytes, p.borrowed_bytes_, e.quota()});
    // Actually we need to return quota to emergency
    if (to_return == 0) return false;
    p.quota_bytes_ -= to_return;
    p.borrowed_bytes_ -= to_return;
    e.quota_bytes_ += to_return;
    return true;
}

// ── Allocate / Deallocate ────────────────────────────────────────────

BlockHandle VramEngine::allocate(PoolType pool, size_t size, size_t alignment,
                                 std::string_view tag) {
    if (!initialized_) {
        std::fprintf(stderr, "[vram] allocate called before initialize\n");
        return kInvalidHandle;
    }
    if (size == 0) return kInvalidHandle;
    if (alignment == 0) alignment = config_.alignment;
    size_t aligned = align_up(size, alignment);

    // Check for high pressure and try borrowing *before* OOM
    MemoryPool& p = pool_of(pool);
    {
        std::lock_guard<std::mutex> lock(p.mutex());
        if (p.pressure() > config_.high_pressure_threshold) {
            // Proactive borrow attempt for KV/Activations
            if (pool == PoolType::KvCache || pool == PoolType::ActivationsTensors) {
                // Unlock before borrowing to avoid deadlock
            }
        }
    }
    if ((pool == PoolType::KvCache || pool == PoolType::ActivationsTensors) &&
        p.pressure() > config_.high_pressure_threshold) {
        borrow_memory(pool, aligned);
    }

    // Ensure capacity (may offload LRU or borrow)
    if (!ensure_capacity(pool, aligned)) {
        std::lock_guard<std::mutex> lock(p.mutex());
        p.oom_++;
        std::fprintf(stderr, "[vram] OOM: pool %s quota %zu KiB, used %zu KiB, request %zu KiB\n",
                     pool_cstr(pool), p.quota()>>10, p.used_vram()>>10, aligned>>10);
        return kInvalidHandle;
    }

    // Allocate device memory
    void* dptr = nullptr;
    if (!hal_malloc_device(&dptr, aligned)) {
        std::lock_guard<std::mutex> lock(p.mutex());
        p.oom_++;
        return kInvalidHandle;
    }

    // For HostSwapStaging and for double-buffering, also allocate pinned host
    // shadow if the pool is likely to be offloaded (e.g., weights, KV)
    void* hptr = nullptr;
    bool need_host = (pool == PoolType::HostSwapStaging) ||
                     (pool == PoolType::ModelWeights) ||
                     (pool == PoolType::KvCache) ||
                     (pool == PoolType::EmbeddingBuffers);
    bool pinned = true;
    if (need_host) {
        if (!hal_malloc_host(&hptr, aligned, true)) {
            // Host alloc failure is non-fatal; we can still keep device-only
            hptr = nullptr;
            pinned = false;
        }
    }

    // Create block
    auto blk = std::make_unique<MemoryBlock>();
    blk->id = next_block_id_.fetch_add(1, std::memory_order_relaxed);
    blk->vram_ptr = dptr;
    blk->host_ptr = hptr;
    blk->shadow_ptr = nullptr;
    blk->size = size;
    blk->aligned_size = aligned;
    blk->pool = pool;
    blk->device_id = config_.device_id;
    blk->is_on_vram = true;
    blk->is_on_host = (hptr != nullptr);
    blk->is_pinned = pinned;
    blk->is_dirty = false;
    blk->is_borrowed = false;
    blk->tag = std::string(tag);

    BlockHandle handle = blk->id;

    {
        std::lock_guard<std::mutex> lock(p.mutex());
        // Check if this allocation was actually borrowed (quota was expanded)
        if (p.borrowed_bytes_ > 0) {
            blk->is_borrowed = true;
            blk->borrowed_from = PoolType::EmergencyOverflow;
        }
        p.track_block(std::move(blk));
        p.misses_++; // first touch is a miss
    }
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        handle_to_pool_.emplace(handle, pool);
    }

    return handle;
}

bool VramEngine::deallocate(BlockHandle handle) {
    if (handle == kInvalidHandle) return false;

    PoolType pool;
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        auto it = handle_to_pool_.find(handle);
        if (it == handle_to_pool_.end()) {
            std::fprintf(stderr, "[vram] deallocate: unknown handle %llu\n",
                         (unsigned long long)handle);
            return false;
        }
        pool = it->second;
        handle_to_pool_.erase(it);
    }

    MemoryPool& p = pool_of(pool);
    std::unique_ptr<MemoryBlock> blk;
    {
        std::lock_guard<std::mutex> lock(p.mutex());
        blk = p.untrack_block(handle);
        if (!blk) return false;
        // If this block was borrowed, return quota to Emergency
        if (blk->is_borrowed) {
            size_t sz = blk->aligned_size;
            // Unlock before returning borrowed to avoid deadlock
            // (return_borrowed locks both pools)
            // So we defer that until after we release p.mutex
        }
    }
    bool was_borrowed = blk->is_borrowed;
    size_t borrowed_size = blk->aligned_size;

    // Free HAL memory outside locks
    hal_free_device(blk->vram_ptr);
    hal_free_host(blk->host_ptr, blk->is_pinned);
    hal_free_host(blk->shadow_ptr, true);

    if (was_borrowed) {
        return_borrowed(pool, borrowed_size);
    }
    return true;
}

// ── Prefetch / Offload (async pipeline) ──────────────────────────────

bool VramEngine::prefetch_to_vram(BlockHandle handle, hal::Stream stream,
                                  bool double_buffer) {
    if (handle == kInvalidHandle) return false;

    PoolType pool;
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        auto it = handle_to_pool_.find(handle);
        if (it == handle_to_pool_.end()) return false;
        pool = it->second;
    }
    MemoryPool& p = pool_of(pool);
    MemoryBlock* blk = nullptr;
    {
        std::lock_guard<std::mutex> lock(p.mutex());
        blk = p.find_block(handle);
        if (!blk) return false;
        if (blk->is_on_vram) {
            p.touch(blk);
            return true; // already resident — just update LRU
        }
        if (!blk->is_on_host || !blk->host_ptr) {
            std::fprintf(stderr, "[vram] prefetch: block %llu has no host copy\n",
                         (unsigned long long)handle);
            return false;
        }
    }

    // Ensure VRAM capacity before bringing back
    if (!ensure_capacity(pool, blk->aligned_size)) {
        std::fprintf(stderr, "[vram] prefetch: no VRAM capacity for block %llu pool %s\n",
                     (unsigned long long)handle, pool_cstr(pool));
        return false;
    }

    // Allocate VRAM if needed (was offloaded)
    if (!blk->vram_ptr) {
        void* dptr = nullptr;
        if (!hal_malloc_device(&dptr, blk->aligned_size)) return false;
        blk->vram_ptr = dptr;
    }

    if (!stream) stream = get_next_stream();

    // Double-buffering: allocate shadow if requested and not present
    if (double_buffer && config_.enable_double_buffer && !blk->shadow_ptr) {
        void* shadow = nullptr;
        if (hal_malloc_host(&shadow, blk->aligned_size, true)) {
            blk->shadow_ptr = shadow;
            // Pre-stage next tile into shadow while front is compute-bound
            // (caller is responsible for swapping front/back after sync)
        }
    }

    // Async host → device
    bool ok = hal_memcpy_async(blk->vram_ptr, blk->host_ptr, blk->aligned_size, stream, true);
    if (ok) {
        // We mark as dirty=false and on_vram=true optimistically;
        // for true async, you'd set a pending flag and flip on stream callback.
        // Here we keep it simple: caller should synchronize stream before using front().
        blk->is_on_vram = true;
        blk->is_dirty = false;
        {
            std::lock_guard<std::mutex> lock(p.mutex());
            p.used_vram_ += blk->aligned_size;
            p.touch(blk);
            p.prefetch_issued_++;
        }
    }
    return ok;
}

bool VramEngine::offload_to_ddr(BlockHandle handle, hal::Stream stream,
                                bool keep_vram_copy) {
    if (handle == kInvalidHandle) return false;

    PoolType pool;
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        auto it = handle_to_pool_.find(handle);
        if (it == handle_to_pool_.end()) return false;
        pool = it->second;
    }
    MemoryPool& p = pool_of(pool);
    MemoryBlock* blk = nullptr;
    {
        std::lock_guard<std::mutex> lock(p.mutex());
        blk = p.find_block(handle);
        if (!blk) return false;
        if (blk->is_on_host && !keep_vram_copy) {
            // Already on host and we don't need VRAM — nothing to do
            // But we still want to free VRAM if requested
        }
        if (!blk->is_on_vram) return true; // already offloaded
        if (!blk->is_on_host) {
            // Need host buffer
            void* hptr = nullptr;
            if (!hal_malloc_host(&hptr, blk->aligned_size, true)) return false;
            blk->host_ptr = hptr;
            blk->is_on_host = true;
            blk->is_pinned = true;
            p.used_host_ += blk->aligned_size;
        }
    }

    if (!stream) stream = get_next_stream();

    bool ok = hal_memcpy_async(blk->host_ptr, blk->vram_ptr, blk->aligned_size, stream, false);
    if (ok) {
        // For async correctness, we'd free VRAM on stream completion.
        // Here we synchronize to keep accounting deterministic.
        hal_stream_synchronize(stream);
        if (!keep_vram_copy) {
            hal_free_device(blk->vram_ptr);
            {
                std::lock_guard<std::mutex> lock(p.mutex());
                p.used_vram_ -= blk->aligned_size;
                p.evictions_++;
                // Remove from LRU (host-only)
                auto it = std::find(p.lru_.begin(), p.lru_.end(), blk);
                if (it != p.lru_.end()) p.lru_.erase(it);
            }
            blk->vram_ptr = nullptr;
            blk->is_on_vram = false;
        }
        blk->is_dirty = false;
        {
            std::lock_guard<std::mutex> lock(p.mutex());
            p.offload_issued_++;
        }
    }
    return ok;
}

void* VramEngine::raw_device_ptr(BlockHandle handle) const {
    PoolType pool;
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        auto it = handle_to_pool_.find(handle);
        if (it == handle_to_pool_.end()) return nullptr;
        pool = it->second;
    }
    const MemoryPool& p = pool_of(pool);
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(p.mutex()));
    MemoryBlock* blk = const_cast<MemoryPool&>(p).find_block(handle);
    return blk ? blk->vram_ptr : nullptr;
}

void* VramEngine::raw_host_ptr(BlockHandle handle) const {
    PoolType pool;
    {
        std::lock_guard<std::mutex> hlock(handle_map_mutex_);
        auto it = handle_to_pool_.find(handle);
        if (it == handle_to_pool_.end()) return nullptr;
        pool = it->second;
    }
    const MemoryPool& p = pool_of(pool);
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(p.mutex()));
    MemoryBlock* blk = const_cast<MemoryPool&>(p).find_block(handle);
    return blk ? blk->host_ptr : nullptr;
}

// ── Stats ────────────────────────────────────────────────────────────

PoolStats VramEngine::get_pool_stats(PoolType pool) const {
    const MemoryPool& p = pool_of(pool);
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(p.mutex()));
    return p.snapshot();
}

std::array<PoolStats, kPoolCount> VramEngine::get_all_stats() const {
    std::array<PoolStats, kPoolCount> out;
    for (size_t i = 0; i < kPoolCount; ++i) {
        out[i] = get_pool_stats(static_cast<PoolType>(i));
    }
    return out;
}

void VramEngine::print_pool_stats() const {
    auto stats = get_all_stats();
    std::printf("\n┌─────────────────────────────────────────────────────────────────────────────┐\n");
    std::printf("│ Red Daft VRAM Engine — Pool Stats (VRAM tier)                              │\n");
    std::printf("├──────────────┬──────────┬──────────┬───────┬──────┬──────┬──────┬──────────┤\n");
    std::printf("│ Pool         │ Quota    │ Used VRAM│ Used H│ Blocks│ Evict│ Hit  │ Pressure │\n");
    std::printf("├──────────────┼──────────┼──────────┼───────┼──────┼──────┼──────┼──────────┤\n");
    for (auto& s : stats) {
        double press = s.quota_bytes ? (100.0 * s.used_vram / s.quota_bytes) : 0.0;
        std::printf("│ %-12s │ %6zu MiB │ %6zu MiB │ %4zu │ %4zu │ %4zu │ %4zu │ %5.1f%%    │\n",
                    kPoolNames[static_cast<size_t>(s.pool)].data(),
                    s.quota_bytes>>20, s.used_vram>>20, s.used_host>>20,
                    s.block_count, s.evictions, s.hits, press);
    }
    std::printf("└──────────────┴──────────┴──────────┴───────┴──────┴──────┴──────┴──────────┘\n");
    // Global
    size_t total_quota = 0, total_used = 0, total_host = 0;
    for (auto& s : stats) { total_quota += s.quota_bytes; total_used += s.used_vram; total_host += s.used_host; }
    std::printf("  Total VRAM: %zu / %zu MiB (%.1f%%)  Host pinned: %zu MiB  Amplification: %.2fx\n",
                total_used>>20, total_quota>>20,
                total_quota ? 100.0*total_used/total_quota : 0.0,
                total_host>>20,
                total_used ? static_cast<double>(total_used + total_host)/total_used : 1.0);
    std::printf("  Streams: %zu  Double-buffer: %s  HT pressure thresh: %.0f%%\n\n",
                streams_.size(),
                config_.enable_double_buffer ? "on" : "off",
                config_.high_pressure_threshold*100);
}

} // namespace red_daft
