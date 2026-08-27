#pragma once
/**
 * red_daft_nano_context.h — Red Daft OS Nano-Context Engine
 * ========================================================================
 * High-density context-window manager for LLM inference.
 *
 * Design philosophy — SEPARATION OF CONTEXT & WEIGHTS:
 *   - Base Model Weights stay STRICTLY in FP16 inside VRAM Pool 0
 *     (ModelWeights). They are never quantized, touched, or moved to a
 *     Nano-Pool.
 *   - ALL dynamic context memory — Input Tokens, KV Cache, Output Stream
 *     tokens — routes to ephemeral, high-density "Nano-Pools" that are
 *     spawned on request start and recycled on generation completion.
 *
 * Nano-Pool compression:
 *   - KV cache entries are dynamically quantized FP16 -> INT4 or INT2 inside
 *     the Nano-Pool buffer (per-channel scale + zero-point), giving up to
 *     4-8x density vs raw FP16 while keeping K/V read path dequantizing.
 *   - An ephemeral Token Stream Ring buffer handles continuous input/output
 *     token traffic with a configurable footprint (default < 50 MB per
 *     active stream, tunable via NanoContextConfig).
 *
 * Memory lifecycle:
 *   - request_begin() spawns a Nano-Pool on the heap / engine arena.
 *   - request_end() deallocates and recycles it back to a free-list pool so
 *     subsequent requests reuse memory instead of churn.
 *   - A free-list keeps N recycled Nano-Pools for low-latency re-spawn.
 *
 * Backends (HAL parity with red_daft_vram):
 *   - DRD_USE_CUDA  -> cudaMalloc/cudaHostAlloc (device + pinned host)
 *   - DRD_USE_ROCM  -> hipMalloc/hipHostAlloc
 *   - CPU fallback  -> malloc (CI / Kaggle / no GPU)
 *
 * Thread-safety: NanoContextEngine is a singleton guarded by a shared_mutex
 * for the registry of active Nano-Pools; each NanoPool owns its own mutex.
 *
 * C++20. License: Red Daft OS Verbatim Distribution License v1.0.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(RD_USE_ROCM) || defined(__HIP_PLATFORM_AMD__)
  #define RD_NANO_BACKEND_ROCM 1
  #include <hip/hip_runtime.h>
#elif defined(RD_USE_CUDA) || __has_include(<cuda_runtime.h>)
  #if __has_include(<cuda_runtime.h>)
    #define RD_NANO_BACKEND_CUDA 1
    #include <cuda_runtime.h>
  #else
    #define RD_NANO_BACKEND_CPU 1
  #endif
#else
  #define RD_NANO_BACKEND_CPU 1
#endif

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// Enums & config
// ─────────────────────────────────────────────────────────────────────
enum class NanoQuantType : uint8_t {
    None = 0,   // keep FP16 (no quantization)
    Int4  = 1,  // 4-bit per-channel (default for KV)
    Int2  = 2,  // 2-bit per-channel (max density, KV long context)
    Count = 3
};

enum class NanoPoolState : uint8_t {
    Free     = 0, // in free-list, recyclable
    Active   = 1, // bound to a live request
    Retiring = 2  // being recycled back to free-list
};

struct NanoQuantStats {
    uint64_t kv_entries        = 0; // total KV entries written
    uint64_t tokens_processed  = 0; // tokens pushed through the stream
    uint64_t bytes_compressed  = 0; // compressed bytes stored
    uint64_t bytes_saved       = 0; // = uncompressed - compressed
    uint64_t quant_ops         = 0; // quantization passes executed
};

struct NanoStreamStats {
    uint64_t tokens_ingested = 0;
    uint64_t tokens_emitted  = 0;
    uint64_t overflows       = 0;   // ring buffer rollover events
};

struct NanoPoolStats {
    uint64_t request_id       = 0;
    size_t   capacity_bytes   = 0;
    size_t   used_bytes       = 0;
    NanoQuantType k_quant     = NanoQuantType::None;
    NanoQuantType v_quant     = NanoQuantType::None;
    size_t   head_size        = 0; // KV head dimension
    size_t   n_layers         = 0;
    NanoQuantStats quant;
    NanoStreamStats stream;
    bool     is_device_resident = false;
};

struct NanoContextConfig {
    size_t   kv_layers        = 32;    // transformer layers
    size_t   kv_heads         = 8;     // KV heads per layer
    size_t   head_dim         = 128;   // head dimension
    size_t   max_tokens       = 4096;  // max context tokens per request
    size_t   stream_capacity  = 24ULL << 20; // token stream ring bytes (< 50 MB)
    NanoQuantType default_k_quant = NanoQuantType::Int4;
    NanoQuantType default_v_quant = NanoQuantType::Int4;
    size_t   free_list_capacity = 8;  // recycled Nano-Pools to keep warm
    bool     enable_device     = true; // use device (cuda/hip) if available
};

// ─────────────────────────────────────────────────────────────────────
// Quantized KV storage inside a Nano-Pool
// ─────────────────────────────────────────────────────────────────────
struct QuantizedKVCell {
    uint8_t* data  = nullptr;      // INT4/INT2 packed, or FP16 raw
    float*   scale = nullptr;      // per-channel scale (per kv layer/head)
    float*   zpt   = nullptr;      // per-channel zero point
    size_t   size  = 0;            // packed bytes for this cell
    size_t   channels = 0;         // number of scale entries
    NanoQuantType type = NanoQuantType::None;
    uint32_t seq_len = 0;          // tokens stored in this cell (rows)
};

// ─────────────────────────────────────────────────────────────────────
// NanoPool — ephemeral, high-density context region
// ─────────────────────────────────────────────────────────────────────
class NanoPool {
public:
    NanoPool(uint64_t req_id, const NanoContextConfig& cfg);
    ~NanoPool();

    NanoPool(const NanoPool&) = delete;
    NanoPool& operator=(const NanoPool&) = delete;

    // KV cache: quantize + store a (layer, head) key/value tensor
    bool         kv_store(size_t layer, size_t head,
                          const float* key, const float* value, size_t seq_tokens,
                          NanoQuantType kq, NanoQuantType vq);
    bool         kv_load(size_t layer, size_t head,
                         float* key_out, float* value_out, size_t seq_tokens) const;

    // Token stream ring: ingest/emit tokens (minimum footprint)
    bool         stream_push(const float* tokens, size_t n);
    bool         stream_pop(float* out, size_t max_n); // drains oldest
    size_t       stream_available() const;

    // Lifecycle
    void         reset();   // reuse for another request
    NanoPoolStats snapshot() const;

    uint64_t     request_id() const { return request_id_; }
    NanoPoolState state() const { return state_; }
    void         set_state(NanoPoolState s) { state_ = s; }
    size_t       capacity() const { return cfg_.kv_layers * cfg_.kv_heads * quant_cell_bytes_; }
    mutable std::mutex mutex_; // public for engine-level locking patterns

private:
    // Per-(layer,head) quantized cells
    std::vector<std::vector<QuantizedKVCell>> cells_; // [layer][head]

    // Token stream ring
    std::vector<float> ring_;          // fixed-capacity ring
    size_t ring_head_ = 0;             // oldest
    size_t ring_tail_ = 0;             // next write
    size_t ring_count_ = 0;

    NanoContextConfig cfg_;
    uint64_t request_id_;
    NanoPoolState state_ = NanoPoolState::Active;
    NanoQuantStats quant_;
    NanoStreamStats stream_;

    size_t quant_cell_bytes_ = 0;      // packed storage for one (layer,head)
    size_t channel_bytes_    = 0;      // per-channel scale/zpt storage

    // Real device (CUDA/HIP) backing — set when a GPU backend is compiled in.
    bool   device_resident_ = false;
    void*  d_kv_   = nullptr;         // device buffer covering all packed KV cells
    void*  d_stream_ = nullptr;       // device token-stream ring buffer
#if defined(RD_NANO_BACKEND_CUDA)
    cudaStream_t d_hstream_ = nullptr; // per-pool async stream (CUDA)
#elif defined(RD_NANO_BACKEND_ROCM)
    hipStream_t  d_hstream_ = nullptr; // per-pool async stream (HIP)
#else
    void*        d_hstream_ [[maybe_unused]] = nullptr;
#endif
    size_t d_kv_bytes_ = 0;           // allocated size of d_kv_
    size_t d_ring_bytes_ = 0;         // allocated size of d_stream_

    // Device HAL helpers (dual-HAL: CUDA / ROCm / CPU-fallback no-op)
    bool dev_alloc(void** p, size_t bytes);
    void dev_free(void* p);
    bool dev_memcpy_htod(void* dst, const void* src, size_t bytes);
    bool dev_memcpy_dtoh(void* dst, const void* src, size_t bytes);
    bool dev_sync();

    // static quantization helpers
    static void quantize_into(const float* src, size_t n, NanoQuantType t,
                              uint8_t* packed, float* scale, float* zpt);
    static void dequantize_from(const uint8_t* packed, const float* scale,
                                const float* zpt, size_t n, NanoQuantType t,
                                float* out);
    static size_t packed_bytes(size_t n, NanoQuantType t);
};

// ─────────────────────────────────────────────────────────────────────
// NanoContextEngine — singleton orchestrator
// ─────────────────────────────────────────────────────────────────────
class NanoContextEngine {
public:
    static NanoContextEngine& instance();

    NanoContextEngine(const NanoContextEngine&) = delete;
    NanoContextEngine& operator=(const NanoContextEngine&) = delete;

    bool initialize(const NanoContextConfig& cfg = {});
    void shutdown();
    bool is_initialized() const { return initialized_; }
    const NanoContextConfig& config() const { return cfg_; }

    // ── Lifecycle ───────────────────────────────────────────────────
    // Spawn a fresh Nano-Pool for a request (from free-list or new).
    uint64_t request_begin();
    // Recycle the pool back to the free-list on generation completion.
    void     request_end(uint64_t req_id);

    // ── KV & stream ops ─────────────────────────────────────────────
    bool kv_store(uint64_t req_id, size_t layer, size_t head,
                  const float* key, const float* value, size_t seq_tokens);
    bool kv_load(uint64_t req_id, size_t layer, size_t head,
                 float* key_out, float* value_out, size_t seq_tokens) const;
    bool stream_push(uint64_t req_id, const float* tokens, size_t n);
    bool stream_pop(uint64_t req_id, float* out, size_t max_n);

    // ── Introspection ──────────────────────────────────────────────
    size_t active_pools() const;
    size_t free_pools() const;
    NanoPoolStats pool_stats(uint64_t req_id) const;
    void   print_stats() const;
    uint64_t total_requests() const { return total_requests_.load(); }

private:
    NanoContextEngine() = default;
    ~NanoContextEngine() { shutdown(); }

    // Direct HAL device alloc/free (independent of vram engine, isolated).
    bool   dev_alloc(void** p, size_t n);
    void   dev_free(void* p);

    NanoContextConfig cfg_{};
    std::unordered_map<uint64_t, std::unique_ptr<NanoPool>> active_;
    std::vector<std::unique_ptr<NanoPool>> free_list_;
    mutable std::shared_mutex registry_mutex_;
    std::atomic<uint64_t> next_req_{1};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<bool> initialized_{false};
};

// ── Free-function convenience wrappers ───────────────────────────────
inline bool nano_initialize(const NanoContextConfig& cfg = {}) {
    return NanoContextEngine::instance().initialize(cfg);
}
inline uint64_t nano_request_begin() {
    return NanoContextEngine::instance().request_begin();
}
inline void nano_request_end(uint64_t id) {
    NanoContextEngine::instance().request_end(id);
}

} // namespace red_daft
