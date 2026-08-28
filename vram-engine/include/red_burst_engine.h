#pragma once
/**
 * red_burst_engine.h — red-burst Ultra-LoRA Engine
 * ==================================================
 * Executes a high-density, 3-second compute spike (target 100% GPU
 * utilization) on CUDA (NVIDIA) or ROCm/HIP (AMD) hardware to dynamically
 * quantize and compress active LoRA adapters and KV-Cache states down to
 * micro-buffers (INT4 / FP4 / INT2), drastically minimizing VRAM footprint
 * during execution.
 *
 * Architecture:
 *   - red_daft_gpu.h : unified CUDA/ROCm HAL (device malloc/memcpy/streams,
 *                      WMMA⇄MFMA tensor-core hooks; CPU fallback for CI).
 *   - 3 parallel streams during the burst:
 *        Stream 0  KvQuantize     — KV Cache dynamic quantization
 *        Stream 1  LoRaProject    — LoRA adapter weight re-projection
 *        Stream 2  PageAlign      — memory page alignment & pool allocation
 *   - burst_watchdog_timer(): isolated watchdog thread that monitors elapsed
 *     time + thermal, throttles GPU clocks back to nominal after the strict
 *     3.00 s cap, and forces fallback to prevent TDR resets / PSU trips.
 *   - Zero allocation inside the compute loop: burst staging buffers are
 *     pre-allocated at configure() time.
 *   - Thread-safe, C++20, POSIX/C++ threads for async safety monitoring.
 *
 * Compute-path model:
 *   Under a real vendor compiler the tensor-core blocks run on WMMA/MFMA;
 *   under g++/clang the reference kernel is a deterministic CPU model using
 *   the same INT4/INT2/FP4 packing so the whole pipeline (streams → quant →
 *   pools → watchdog) is runnable and verifiable in CI without a GPU.
 *
 * License: Red Daft OS Verbatim Distribution License v1.0 (see /LICENSE).
 */

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "red_daft_gpu.h"
#include "red_daft_lora_manager.h"
#include "red_daft_vram.h"

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// Micro-precision formats
// ─────────────────────────────────────────────────────────────────────
enum class BurstQuantType : uint8_t {
    Int4 = 0,   // 4-bit signed, per-channel scale (6x vs fp16)
    Int2 = 1,   // 2-bit unsigned, per-channel scale (8x vs fp16)
    Fp4  = 2,   // 4-bit e2m1 float (7x vs fp16)
    COUNT
};
inline constexpr std::array<std::string_view, 3> kBurstQuantNames = {
    "Int4", "Int2", "Fp4"
};
inline std::string_view to_string(BurstQuantType q) {
    return kBurstQuantNames[static_cast<size_t>(q)];
}

// Stream role within the 3-second burst
enum class BurstStream : uint8_t {
    KvQuantize = 0,    // Stream 0: KV Cache dynamic quantization
    LoRaProject= 1,    // Stream 1: LoRA adapter weight re-projection
    PageAlign  = 2,    // Stream 2: memory page alignment & pool allocation
    COUNT
};
inline constexpr std::array<std::string_view, 3> kBurstStreamNames = {
    "KvQuantize", "LoRaProject", "PageAlign"
};

// ─────────────────────────────────────────────────────────────────────
// Configuration / source descriptors
// ─────────────────────────────────────────────────────────────────────
struct BurstConfig {
    double      burst_seconds      = 3.0;          // strict target duration
    int         device_id          = 0;
    BurstQuantType lora_quant      = BurstQuantType::Int4;
    BurstQuantType kv_quant        = BurstQuantType::Int4;
    float       target_gpu_percent = 100.0f;       // spike target
    float       throttle_temp_c    = 85.0f;        // thermal trip threshold
    size_t      stage_io_bytes     = 1 << 22;      // pre-allocated staging (4 MiB)
    size_t      page_align_bytes   = 4096;         // memory page alignment
    bool        enable_watchdog    = true;
    bool        route_to_pools     = true;         // push into 10-pool system
};

// One KV region to compress (in practice the Nano-Context KV cells)
struct BurstKvSource {
    const float*  data = nullptr;    // FP32 (dequantized view) or FP16 halfs
    size_t        elements = 0;
    size_t        head_dim = 0;
    PoolType      dest_pool = PoolType::KvCache;
    bool          is_half = false;   // if true, data points to __half
};

// A single LoRA adapter selected for burst re-projection/compression
struct BurstLoraTarget {
    uint32_t  adapter_id = 0;
    LoRaPool  pool = LoRaPool::SystemControl;
    PoolType  dest_pool = PoolType::KvCache;
};

// ─────────────────────────────────────────────────────────────────────
// Metrics / result
// ─────────────────────────────────────────────────────────────────────
struct BurstStreamStats {
    size_t      bytes_in = 0;
    size_t      bytes_out = 0;
    size_t      elems = 0;
    double      seconds = 0.0;
    size_t      mflops = 0;
    bool        completed = false;
};

struct BurstMetrics {
    std::array<BurstStreamStats, 3> stream;      // per-stream totals
    size_t combined_bytes_in = 0;
    size_t combined_bytes_out = 0;               // compressed bytes each pool gets
    std::array<size_t, 10> pool_bytes{};         // routed into pools 0..9
    double  peak_elapsed_s = 0.0;
    double  gpu_percent = 0.0;
    double  peak_temp_c = 0.0;
    bool    watchdog_engaged = false;
    bool    throttled = false;
    size_t  quant_ops = 0;                       // int4/int2/fp4 packing passes
};

struct BurstResult {
    bool    ok = false;
    BurstMetrics metrics;
    std::string error;
};

// ─────────────────────────────────────────────────────────────────────
// RedBurstEngine
// ─────────────────────────────────────────────────────────────────────
class RedBurstEngine {
public:
    RedBurstEngine();
    ~RedBurstEngine();
    RedBurstEngine(const RedBurstEngine&) = delete;
    RedBurstEngine& operator=(const RedBurstEngine&) = delete;

    // Pre-allocate burst staging buffers (zero-alloc inside the loop).
    // Returns false if staging could not be reserved on the GPU/CPU HAL.
    bool configure(const BurstConfig& cfg);

    // Public byte-count query (for tests / CLI introspection).
    static size_t packed_bytes_for(size_t elems, BurstQuantType q);

    // Synchronous burst of configured duration. Spawns the 3 parallel stream
    // workers + the isolated watchdog, joins all, and returns the metrics.
    BurstResult execute_ultra_burst();

    // --- per-stream work feeders (called by stream dispatchers) ---
    // Queue a KV region to quantize on Stream 0.
    void submit_kv(BurstKvSource kv);
    // Queue a LoRA adapter to re-project/compress on Stream 1.
    bool submit_lora(const BurstLoraTarget& tgt);
    // Route `<elements>` quantized bytes to `<pool>` on Stream 2.
    void submit_page(uint8_t* data, size_t bytes, PoolType pool);

    // IoC interfaces to the wider system (caller-supplied adapters)
    using LoraFindFn   = std::function<LoRaAdapter*(uint32_t)>;
    using PoolHostPush = std::function<void(PoolType, const void*, size_t)>;

    void set_lora_find_fn(LoraFindFn fn) { lora_find_ = std::move(fn); }
    void set_pool_push_fn(PoolHostPush fn) { pool_push_ = std::move(fn); }

    // Watchdog: exposed for CLI/testing introspection
    bool watchdog_engaged() const { return engaged_.load(); }
    bool throttled() const { return throttled_.load(); }
    double elapsed_seconds() const { return elapsed_s_.load(); }
    double peak_temp() const { return peak_temp_.load(); }

    const BurstConfig& config() const { return cfg_; }
    const BurstMetrics& metrics() const { return metrics_; }

private:
    // --- dispatch / kernel workers ---
    void stream_kv_worker(gpu::Stream s);          // Stream 0
    void stream_lora_worker(gpu::Stream s);        // Stream 1
    void stream_page_worker(gpu::Stream s);        // Stream 2
    void burst_watchdog_timer();                   // isolated safety thread

    // micro-precision packing (reference CPU model of WMMA/MFMA path)
    static size_t pack_into(void* dst, const void* src, size_t elems,
                            BurstQuantType q, bool src_is_half,
                            float* scale, float* zpt);
    static size_t packed_bytes(size_t elems, BurstQuantType q);
    static size_t kv_elements_touched(const BurstKvSource& kv) { return kv.elements; }

    // thermal/GPU clock throttling hooks
    static bool read_gpu_temp(float& c);
    static bool throttle_gpu_to_nominal();

    BurstConfig cfg_;
    BurstMetrics metrics_;

    LoraFindFn   lora_find_;
    PoolHostPush pool_push_;

    // pre-allocated staging buffers (zero alloc inside loop)
    std::vector<uint8_t> stage_kv_in_, stage_kv_out_, stage_kv_scale_, stage_kv_zpt_;
    std::vector<uint8_t> stage_lo_in_, stage_lo_out_, stage_lo_scale_, stage_lo_zpt_;
    std::vector<float>   scratch_;

    // queues
    std::vector<BurstKvSource> kv_queue_;
    std::vector<BurstLoraTarget> lora_queue_;
    std::vector<std::pair<uint8_t*, std::pair<size_t, PoolType>>> page_queue_;
    std::mutex qmtx_;

    // watchdog / burst control
    std::atomic<double> elapsed_s_{0.0};
    std::atomic<double> peak_temp_{0.0};
    std::atomic<float>  burst_started_{0};
    std::atomic<bool>   engaged_{false};
    std::atomic<bool>   throttled_{false};
    std::atomic<bool>   burst_active_{false};
    std::atomic<double> gpu_percent_{0.0};

    std::vector<std::thread> threads_;
    std::atomic<uint32_t> quant_ops_{0};
};

// free-function convenience wrapper
RedBurstEngine& burst_engine();

} // namespace red_daft
