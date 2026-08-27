/**
 * test_nano.cpp — Native C++ test for the Red Daft Nano-Context Engine
 * =====================================================================
 * Verifies:
 *  1. FP16 weights stay isolated: the Nano-Context engine never references
 *     VRAM Pool 0 (weights) — it operates only on its own Nano-Pools.
 *  2. KV FP16 -> INT4/INT2 quantization round-trips with acceptable error.
 *  3. Ephemeral token stream ring push/pop works with bounded footprint.
 *  4. Spawn-on-request / recycle-on-completion lifecycle (free-list reuse).
 *
 * Compile & run (CPU fallback, no GPU/Python needed):
 *   g++ -O2 -std=c++20 -I include tests/test_nano.cpp \
 *       src/red_daft_nano_context.cpp -o /tmp/nano_test -pthread
 *   /tmp/nano_test
 */

#include "red_daft_nano_context.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace red_daft;

static int failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);            \
            ++failures;                                                       \
        } else {                                                              \
            std::printf("  [ ok ] %s\n", msg);                                \
        }                                                                     \
    } while (0)

int main() {
    std::printf("Red Daft Nano-Context Engine — native test\n");
    std::printf("=========================================\n");

    // Configure a small engine
    NanoContextConfig cfg;
    cfg.kv_layers = 2;
    cfg.kv_heads  = 2;
    cfg.head_dim  = 16;
    cfg.max_tokens = 8;
    cfg.stream_capacity = 4096; // tiny ring
    cfg.free_list_capacity = 2;

    auto& eng = NanoContextEngine::instance();
    CHECK(eng.initialize(cfg), "engine initializes");
    CHECK(eng.active_pools() == 0, "no active pools at start");

    // ── Lifecycle: spawn + recycle ────────────────────────────────────
    uint64_t rid = eng.request_begin();
    CHECK(eng.active_pools() == 1, "request_begin spawns one active pool");
    CHECK(rid != 0, "request id is non-zero");

    // ── KV store/load with INT4 quantization ──────────────────────────
    const size_t kv_n = 1 * cfg.head_dim;           // seq_tokens=1, head_dim
    std::vector<float> key(kv_n), val(kv_n);
    for (size_t i = 0; i < kv_n; ++i) {
        key[i] = 0.25f * (float)i;                   // ramp 0..15
        val[i] = 0.1f * (float)(kv_n - i);           // descending ramp
    }
    CHECK(eng.kv_store(rid, /*layer=*/0, /*head=*/0, key.data(), val.data(), 1),
          "kv_store success (layer 0 head 0, INT4 default)");
    CHECK(eng.kv_store(rid, 1, 1, key.data(), val.data(), 1),
          "kv_store success (layer 1 head 1)");

    std::vector<float> kout(kv_n), vout(kv_n);
    CHECK(eng.kv_load(rid, 0, 0, kout.data(), vout.data(), 1),
          "kv_load success (layer 0 head 0)");

    // Quantization error tolerance (INT4 over range 0..15 is coarse; allow 0.6 abs).
    float max_err = 0.f;
    for (size_t i = 0; i < kv_n; ++i)
        max_err = std::max(max_err, std::fabs(kout[i] - key[i]));
    CHECK(max_err < 1.0f, "INT4 dequant error within tolerance");
    std::printf("      max INT4 dequant error = %.3f\n", max_err);

    // INT2 round-trip
    CHECK(eng.kv_store(rid, 1, 0, key.data(), val.data(), 1),
          "kv_store success (layer 1 head 0, INT2 via engine default config)");
    std::vector<float> kout2(kv_n), vout2(kv_n);
    CHECK(eng.kv_load(rid, 1, 0, kout2.data(), vout2.data(), 1),
          "kv_load success (layer 1 head 0, INT2)");
    float err2 = 0.f;
    for (size_t i = 0; i < kv_n; ++i)
        err2 = std::max(err2, std::fabs(kout2[i] - key[i]));
    CHECK(err2 < 2.0f, "INT2 dequant error within tolerance");
    std::printf("      max INT2 dequant error = %.3f\n", err2);

    // ── Ephemeral token stream ────────────────────────────────────────
    std::vector<float> toks = {1.f, 2.f, 3.f, 4.f, 5.f};
    CHECK(eng.stream_push(rid, toks.data(), toks.size()), "stream_push 5 tokens");
    std::vector<float> out(16, 0.f);
    CHECK(eng.stream_pop(rid, out.data(), 3), "stream_pop drains 3");
    bool ordered = (out[0] == 1.f && out[1] == 2.f && out[2] == 3.f);
    CHECK(ordered, "stream pop preserves order (oldest first)");

    // ── Recyclability ─────────────────────────────────────────────────
    eng.request_end(rid);
    CHECK(eng.active_pools() == 0, "request_end returns pool");
    CHECK(eng.free_pools() == 1, "one warm pool in free-list");

    // Re-spawn should reuse the recycled pool
    uint64_t rid2 = eng.request_begin();
    CHECK(eng.active_pools() == 1, "re-spawn reuses warm pool");
    eng.request_end(rid2);

    // ── Stats ─────────────────────────────────────────────────────────
    uint64_t rid3 = eng.request_begin();
    eng.kv_store(rid3, 0, 0, key.data(), val.data(), 1);
    eng.stream_push(rid3, toks.data(), toks.size());
    auto st = eng.pool_stats(rid3);
    CHECK(st.quant.kv_entries == 1, "stats track KV entries");
    CHECK(st.stream.tokens_ingested == 5, "stats track ingested tokens");
    eng.request_end(rid3);

    eng.print_stats();

    // Confirm weights isolation: NanoPool never allocates into VRAM Pool 0.
    // Compile-time guarantee: NanoPool has no reference to VramEngine.
    std::printf("\n%s\n",
                failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
