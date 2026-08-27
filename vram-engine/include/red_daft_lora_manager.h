#pragma once
/**
 * red_daft_lora_manager.h — Modular LoRa Brain Engine for Red Daft OS
 * =================================================================
 * Manages microsecond-level hot-swapping of multiple LoRa adapters
 * (10 MB – 50 MB each) between System DDR4/DDR5 RAM and GPU VRAM
 * across our 10 OS Memory Pools. Keeps VRAM usage minimal while
 * running a shared Base Model (1B – 3B).
 *
 * Architecture:
 *   Pool 0: Shared Base Model Weights (static backbone, never swapped)
 *   Pool 1: OS Control & System Agent LoRa
 *   Pool 2: Reasoning & Logic LoRa
 *   Pool 3: Coding & Syntax LoRa
 *   Pool 4: Conversation & Language LoRa
 *   Pools 5–9: Dynamic KV Cache, Activations, Pinned DDR Offload,
 *              Emergency Pools (managed by VramEngine)
 *
 * LoRa lifecycle:
 *   Unregistered → InDDR → Loading → Active → Evicting → InDDR ...
 *                    ↑                                    │
 *                    └────────────────────────────────────┘
 *
 * Zero-copy / async hot-swapping:
 *   - Inactive LoRa weights live in Host Pinned Memory
 *     (cudaMallocHost / hipHostMalloc) via VramEngine.
 *   - Async loader uses CUDA/HIP Streams (cudaMemcpyAsync) to
 *     pre-fetch the required LoRa adapter *before* the compute
 *     layer executes, avoiding GPU stalls.
 *   - SGMV-style dynamic weight patching applies/removes LoRa
 *     rank matrices ($A$ and $B$) on-the-fly without modifying
 *     the static base model weights in Pool 0.
 *
 * LRU Eviction:
 *   If VRAM is constrained, automatically evicts the Least Recently
 *   Used LoRa adapter back to System DDR RAM, retaining only the
 *   active adapter(s) in Pools 1–4.
 *
 * Cross-platform HAL:
 *   Reuses VramEngine's HAL: macro-switchable at compile time:
 *   - NVIDIA:  -DRD_USE_CUDA
 *   - AMD:     -DRD_USE_ROCM
 *   - CPU:     (no flag) → malloc simulation
 *
 * Thread-safety: All public APIs are protected by std::shared_mutex
 * with per-adapter granularity for concurrent reads.
 *
 * C++20, -std=c++20.
 *
 * Author: Red Daft OS — Systems Performance Team
 * License: Red Daft OS Verbatim Distribution License v1.0 (see /LICENSE)
 */

#include "red_daft_vram.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// LoRa Pool Mapping (spec §1)
// ─────────────────────────────────────────────────────────────────────
enum class LoRaPool : uint8_t {
    SystemControl   = 1,  // Pool 1: OS Control & System Agent LoRa
    ReasoningLogic  = 2,  // Pool 2: Reasoning & Logic LoRa
    CodingSyntax    = 3,  // Pool 3: Coding & Syntax LoRa
    ConversationLang= 4,  // Pool 4: Conversation & Language LoRa
    COUNT           = 4
};
constexpr size_t kLoRaPoolCount = 4;

inline constexpr std::array<std::string_view, kLoRaPoolCount> kLoRaPoolNames = {
    "SystemControl", "ReasoningLogic", "CodingSyntax", "ConversationLang"
};
inline std::string_view to_string(LoRaPool p) {
    return kLoRaPoolNames[static_cast<size_t>(p) - 1];
}

// ─────────────────────────────────────────────────────────────────────
// LoRa Adapter State Machine
// ─────────────────────────────────────────────────────────────────────
enum class LoRaState : uint8_t {
    Unregistered,  // In registry but not loaded to any memory
    InDDR,         // Weights reside in pinned host memory only
    Loading,       // Async host→VRAM transfer in progress
    Active,        // Resident in VRAM, ready for inference
    Evicting,      // Async VRAM→host transfer in progress
    Evicted        // Moved to host, VRAM freed
};

inline constexpr const char* to_string(LoRaState s) {
    switch (s) {
        case LoRaState::Unregistered: return "Unregistered";
        case LoRaState::InDDR:        return "InDDR";
        case LoRaState::Loading:      return "Loading";
        case LoRaState::Active:       return "Active";
        case LoRaState::Evicting:     return "Evicting";
        case LoRaState::Evicted:      return "Evicted";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────────────
// LoRa Adapter — one LoRA module ($A$ down-proj, $B$ up-proj)
// ─────────────────────────────────────────────────────────────────────
struct LoRaAdapter {
    uint32_t    id = 0;
    std::string name;
    LoRaPool    pool = LoRaPool::SystemControl;

    // LoRA dimensions
    uint32_t    rank = 0;          // rank $r$ (typically 4–256)
    uint32_t    in_features = 0;   // input dimension
    uint32_t    out_features = 0;  // output dimension

    // Matrix sizes in bytes (A: rank×in_features, B: out_features×rank)
    size_t      a_size = 0;        // bytes for $A$ matrix
    size_t      b_size = 0;        // bytes for $B$ matrix
    size_t      total_size = 0;    // total adapter bytes

    // Device pointers (VRAM)
    void*       d_A = nullptr;     // $A$ matrix on GPU
    void*       d_B = nullptr;     // $B$ matrix on GPU

    // Host pointers (pinned DDR — always valid after registration)
    void*       h_A = nullptr;     // $A$ matrix on host (pinned)
    void*       h_B = nullptr;     // $B$ matrix on host (pinned)
    bool        owns_host = false; // true if we allocated h_A/h_B

    // State
    LoRaState   state = LoRaState::Unregistered;

    // Async stream for this adapter's transfers
    hal::Stream stream = nullptr;

    // LRU tracking
    mutable std::chrono::steady_clock::time_point last_access;
    mutable uint32_t access_count = 0;

    // Synchronization
    mutable std::mutex mtx;

    // Helpers
    bool is_on_vram() const { return d_A && d_B; }
    bool is_on_host() const { return h_A && h_B; }
    size_t element_count() const { return (size_t)rank * in_features + (size_t)out_features * rank; }
};

// ─────────────────────────────────────────────────────────────────────
// LoRa Registry — thread-safe adapter lookup & lifecycle tracking
// ─────────────────────────────────────────────────────────────────────
class LoRaRegistry {
public:
    LoRaRegistry();
    ~LoRaRegistry();

    LoRaRegistry(const LoRaRegistry&) = delete;
    LoRaRegistry& operator=(const LoRaRegistry&) = delete;
    LoRaRegistry(LoRaRegistry&&) = delete;

    // ── Registration ──────────────────────────────────────────────

    // Register adapter with pre-allocated host weights (caller owns data until copy)
    uint32_t register_adapter(
        std::string_view name,
        LoRaPool pool,
        uint32_t rank,
        uint32_t in_features,
        uint32_t out_features,
        const float* host_A,
        const float* host_B,
        hal::Stream stream = nullptr
    );

    // Register adapter from raw byte buffers (reinterpreted as float)
    uint32_t register_adapter_raw(
        std::string_view name,
        LoRaPool pool,
        uint32_t rank,
        uint32_t in_features,
        uint32_t out_features,
        const void* raw_A, size_t raw_a_bytes,
        const void* raw_B, size_t raw_b_bytes,
        hal::Stream stream = nullptr
    );

    // Register adapter with no data — allocate empty pinned buffers (for lazy loading)
    uint32_t register_empty(
        std::string_view name,
        LoRaPool pool,
        uint32_t rank,
        uint32_t in_features,
        uint32_t out_features,
        hal::Stream stream = nullptr
    );

    void unregister(uint32_t id);

    // ── Lookup ────────────────────────────────────────────────────

    LoRaAdapter* find_by_id(uint32_t id);
    const LoRaAdapter* find_by_id(uint32_t id) const;
    LoRaAdapter* find_by_name(std::string_view name);
    LoRaAdapter* find_by_pool(LoRaPool pool);
    bool has_pool(LoRaPool pool) const;

    // ── LRU ───────────────────────────────────────────────────────

    void touch(uint32_t id);
    LoRaAdapter* lru_victim(); // returns adapter with oldest last_access

    // ── State ─────────────────────────────────────────────────────

    void set_state(uint32_t id, LoRaState state);

    // ── Iteration / Stats ─────────────────────────────────────────

    size_t count() const;
    size_t active_count() const;
    size_t vram_count() const;
    size_t total_vram_bytes() const;
    size_t total_host_bytes() const;

    std::vector<LoRaAdapter*> all_adapters();
    std::vector<const LoRaAdapter*> all_adapters() const;

    mutable std::shared_mutex mutex;

private:
    std::vector<std::unique_ptr<LoRaAdapter>> adapters_;
    std::unordered_map<uint32_t, size_t>  id_idx_;
    std::unordered_map<std::string, size_t> name_idx_;
    std::array<size_t, 5> pool_idx_;  // indexed by LoRaPool (1..4), 0 unused
    std::atomic<uint32_t> next_id_{1};
};

// ─────────────────────────────────────────────────────────────────────
// LoRa Swapper — async hot-swap engine with LRU eviction
// ─────────────────────────────────────────────────────────────────────
class LoRaSwapper {
public:
    explicit LoRaSwapper(LoRaRegistry& registry, VramEngine& engine);
    ~LoRaSwapper();

    LoRaSwapper(const LoRaSwapper&) = delete;
    LoRaSwapper& operator=(const LoRaSwapper&) = delete;

    // ── Core Operations ───────────────────────────────────────────

    // Async hot-swap: pre-fetch adapter to VRAM, evict current if needed
    // Returns immediately; transfer runs on stream. Check with event/query.
    bool hot_swap_async(uint32_t target_id, hal::Stream stream = nullptr);

    // Convenience: swap to adapter in named pool (kills current active if different pool)
    bool swap_to_pool(LoRaPool pool, hal::Stream stream = nullptr);

    // Convenience named swaps
    bool swap_to_system_lora(hal::Stream s = nullptr)   { return swap_to_pool(LoRaPool::SystemControl, s); }
    bool swap_to_reasoning_lora(hal::Stream s = nullptr) { return swap_to_pool(LoRaPool::ReasoningLogic, s); }
    bool swap_to_coding_lora(hal::Stream s = nullptr)    { return swap_to_pool(LoRaPool::CodingSyntax, s); }
    bool swap_to_conversation_lora(hal::Stream s = nullptr) { return swap_to_pool(LoRaPool::ConversationLang, s); }

    // Pre-activate: load adapter to VRAM without marking it active
    // (useful for double-buffering: prefetch next layer's LoRa while current runs)
    bool pre_activate(uint32_t id, hal::Stream stream = nullptr);

    // SGMV-style weight patching: apply LoRa rank matrices on-the-fly
    // Computes output = base_output + B @ A @ input without modifying base weights.
    // For CPU fallback, performs matrix multiply directly.
    // For GPU, this issues the SGMV kernel (or memcpy the patch into place).
    bool apply_lora_weights(uint32_t adapter_id,
                            void* base_output, const void* base_input,
                            size_t batch_size = 1);

    // Evict specific adapter from VRAM to DDR (async)
    bool evict_adapter(uint32_t id, hal::Stream stream = nullptr);

    // Evict LRU adapter from VRAM to DDR
    bool evict_lru_lora(hal::Stream stream = nullptr);

    // Evict all non-active adapters from VRAM
    size_t evict_all_except_active(hal::Stream stream = nullptr);

    // ── Introspection ─────────────────────────────────────────────

    uint32_t active_adapter_id() const { return active_id_.load(std::memory_order_relaxed); }
    LoRaAdapter* active_adapter();
    bool is_in_vram(uint32_t id) const;

    // ── Synchronization ───────────────────────────────────────────

    void synchronize();
    bool wait_adapter_ready(uint32_t id, int timeout_ms = 5000);

    // ── Stats ─────────────────────────────────────────────────────

    struct SwapStats {
        uint64_t total_swaps = 0;
        uint64_t total_evictions = 0;
        uint64_t total_prefetches = 0;
        uint64_t total_bytes_transferred = 0;
        double   total_swap_time_us = 0.0;
        double   avg_swap_time_us = 0.0;
    };

    SwapStats stats() const;
    void reset_stats();
    void print_stats() const;

private:
    LoRaRegistry& registry_;
    VramEngine&   engine_;
    std::atomic<uint32_t> active_id_{0};
    mutable std::shared_mutex mtx_;
    SwapStats stats_;

    // Internal
    bool ensure_vram_capacity(size_t bytes_needed, hal::Stream stream);
    bool async_host_to_device(void* dst, const void* src, size_t bytes, hal::Stream stream);
    bool async_device_to_host(void* dst, const void* src, size_t bytes, hal::Stream stream);
    bool sync_memcpy(void* dst, const void* src, size_t bytes, bool to_device);
    void record_swap_time(std::chrono::high_resolution_clock::time_point start);
};

// ─────────────────────────────────────────────────────────────────────
// Free-function convenience wrappers
// ─────────────────────────────────────────────────────────────────────
inline bool lora_hot_swap(LoRaSwapper& sw, uint32_t id) {
    return sw.hot_swap_async(id);
}
inline bool lora_swap_to_coding(LoRaSwapper& sw) {
    return sw.swap_to_coding_lora();
}
inline bool lora_swap_to_reasoning(LoRaSwapper& sw) {
    return sw.swap_to_reasoning_lora();
}

} // namespace red_daft
