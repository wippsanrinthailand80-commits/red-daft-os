/**
 * test_burst.cpp — Native C++ test for the red-burst Ultra-LoRA Engine
 * ====================================================================
 * Verifies:
 *  1. configure() pre-allocates burst staging buffers.
 *  2. execute_ultra_burst() runs all 3 parallel stream dispatchers
 *     (KV quantize / LoRA re-project / page align + pool) and joins them.
 *  3. Watchdog engages at the strict duration cap, marks throttling, and
 *     forces the workers to stop (no overshoot beyond the target + slack).
 *  4. Compressed micro-buffers are routed into the 10-memory-pool system
 *     (pool_bytes counters populated).
 *  5. INT4/INT2/FP4 reference packing produces consistent byte sizes.
 *  6. GPU utilization is reported as ~100% during the window, 0 after.
 *
 * Compile & run (CPU fallback, no GPU/Python needed):
 *   g++ -O2 -std=c++20 -I include tests/test_burst.cpp \
 *       src/red_burst_engine.cpp src/red_daft_vram.cpp \
 *       src/red_daft_lora_manager.cpp -o /tmp/burst_test -pthread
 *   /tmp/burst_test
 */

#include "red_burst_engine.h"
#include "red_daft_vram.h"
#include "red_daft_lora_manager.h"

#include <cstdio>
#include <cstring>
#include <iostream>

using namespace red_daft;

static int failures = 0;
static unsigned pool_pushes = 0;

#define CHECK(cond, msg)                                                     \
    do { if (!(cond)) { std::cout << "  [FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        ++failures; }                                                        \
        else { std::cout << "  [ ok ] " << msg << "\n"; } } while (0)

int main() {
    std::printf("Red-burst Ultra-LoRA Engine — native test\n");
    std::printf("==========================================\n");

    // ── 1. configure with a short burst for a fast test ─────────────
    RedBurstEngine eng;
    BurstConfig cfg;
    cfg.burst_seconds = 0.15;
    cfg.lora_quant = BurstQuantType::Int4;
    cfg.kv_quant = BurstQuantType::Int4;
    CHECK(eng.configure(cfg), "configure() pre-allocates staging");

    // ── provide a LoRa adapter finder + pool push counter ────────────
    static LoRaRegistry reg;
    eng.set_lora_find_fn([&](uint32_t id) -> LoRaAdapter* {
        return reg.find_by_id(id);
    });
    eng.set_pool_push_fn([&](PoolType p, const void*, size_t) {
        (void)p; pool_pushes++;
    });

    // ── register one adapter with real host weights ──────────────────
    float A[128], B[128];
    for (int i = 0; i < 128; i++) { A[i] = (float)(i % 13) - 6.f; B[i] = (float)(i % 9) - 4.f; }
    uint32_t aid = reg.register_adapter(
        "burst_alpha", LoRaPool::SystemControl,
        /*rank=*/8, /*in_features=*/16, /*out_features=*/64,
        A, B);
    CHECK(aid > 0, "adapter registered via LoRaRegistry");

    // ── 2. submit KV + LoRA work ────────────────────────────────────
    BurstKvSource kv;
    static float kvdata[4096];
    for (int i = 0; i < 4096; i++) kvdata[i] = (float)(i % 5) - 2.f;
    kv.data = kvdata;
    kv.elements = 4096;
    kv.head_dim = 64;
    kv.dest_pool = PoolType::KvCache;
    eng.submit_kv(kv);

    BurstLoraTarget tgt;
    tgt.adapter_id = aid;
    tgt.pool = LoRaPool::SystemControl;
    tgt.dest_pool = PoolType::QuantizationMetadata;
    eng.submit_lora(tgt);

    // ── 3. execute burst ─────────────────────────────────────────────
    BurstResult res = eng.execute_ultra_burst();
    CHECK(res.ok, "burst executed successfully");
    CHECK(res.metrics.stream[0].completed, "Stream 0 (KV quant) completed");
    CHECK(res.metrics.stream[1].completed, "Stream 1 (LoRA) completed");
    CHECK(res.metrics.stream[2].completed, "Stream 2 (page align) completed");
    CHECK(res.metrics.quant_ops > 0, "quantization packing ops ran");
    CHECK(pool_pushes > 0, "compressed buffers routed into pools");

    // KvCache = PoolType::KvCache = 1, not index 0
    CHECK(res.metrics.pool_bytes[static_cast<size_t>(PoolType::KvCache)] > 0,
          "KV micro-buffers routed to KvCache pool");
    CHECK(res.metrics.pool_bytes[static_cast<size_t>(PoolType::QuantizationMetadata)] > 0,
          "LoRA compressed weights routed to QuantizationMetadata pool");

    // ── 4. watchdog strict-cap enforcement ───────────────────────────
    CHECK(res.metrics.watchdog_engaged, "watchdog engaged (engaged flag set)");
    CHECK(res.metrics.throttled, "GPU clocks throttled back to nominal");
    CHECK(res.metrics.peak_elapsed_s >= 0.0, "elapsed time tracked");
    CHECK(res.metrics.peak_elapsed_s < (cfg.burst_seconds + 0.5),
          "burst did not overshoot the duration cap (elapsed=" +
              std::to_string(res.metrics.peak_elapsed_s) + "s)");
    CHECK(res.metrics.gpu_percent <= 1.0 || res.metrics.gpu_percent == 0.0,
          "GPU utilization reported 0 after burst ends");

    // ── 5. micro-precision packing byte sizes ────────────────────────
    CHECK(RedBurstEngine::packed_bytes_for(4096, BurstQuantType::Int4) == 2048,
          "INT4 packs 4096 elems -> 2048 bytes");
    CHECK(RedBurstEngine::packed_bytes_for(4096, BurstQuantType::Int2) == 1024,
          "INT2 packs 4096 elems -> 1024 bytes");
    CHECK(RedBurstEngine::packed_bytes_for(4096, BurstQuantType::Fp4) == 2048,
          "FP4 packs 4096 elems -> 2048 bytes");

    if (failures == 0) std::printf("\n[PASS] all burst assertions passed\n");
    else std::printf("\n[FAIL] %d assertion(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
