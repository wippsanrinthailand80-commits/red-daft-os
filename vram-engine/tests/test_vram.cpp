/**
 * test_vram.cpp — Native C++ smoke test for Red Daft VRAM Engine
 * Build: g++ -std=c++20 -I../include ../src/red_daft_vram.cpp test_vram.cpp -o vram_test
 * Or via CMake: make vram_test && ./vram_test
 */
#include "red_daft_vram.h"
#include <cassert>
#include <cstdio>

using namespace red_daft;

int main() {
    printf("=== Red Daft VRAM Engine — C++ Smoke ===\n");

    EngineConfig cfg;
    cfg.vram_budget_bytes = 512ULL << 20; // 512 MiB for test
    cfg.host_budget_bytes = 1024ULL << 20;
    cfg.num_streams = 2;
    cfg.enable_double_buffer = true;

    auto& eng = VramEngine::instance();
    assert(eng.initialize(cfg));
    printf("[test] initialize OK\n");

    // 1) Allocate one block per pool (1 MiB each)
    std::vector<BlockHandle> handles;
    for (int p = 0; p < 10; ++p) {
        BlockHandle h = eng.allocate(static_cast<PoolType>(p), 1<<20, 256, "test_pool_" + std::to_string(p));
        printf("[test] pool %d %-22s %s (handle %llu)\n",
               p, kPoolNames[p].data(), h ? "OK" : "OOM", (unsigned long long)h);
        assert(h != kInvalidHandle);
        handles.push_back(h);
    }

    // 2) Prefetch / offload round-trip
    for (auto h : handles) {
        bool ok = eng.offload_to_ddr(h, nullptr, false);
        assert(ok);
    }
    printf("[test] offload all to DDR OK\n");
    for (auto h : handles) {
        bool ok = eng.prefetch_to_vram(h, nullptr, true);
        assert(ok);
    }
    printf("[test] prefetch all to VRAM OK\n");

    // 3) Borrowing: Fill KV pool to pressure >85%, then borrow from Emergency
    {
        auto s = eng.get_pool_stats(PoolType::EmergencyOverflow);
        size_t emergency_free_before = s.quota_bytes - s.used_vram;
        // Allocate a large KV block to trigger pressure
        BlockHandle big = eng.allocate(PoolType::KvCache, s.quota_bytes, 256, "kv_stress");
        // May borrow; just check it doesn't crash
        eng.borrow_memory(PoolType::KvCache, 1<<20);
        if (big) eng.deallocate(big);
        printf("[test] borrow test OK (emergency free before %zu KiB)\n", emergency_free_before>>10);
    }

    // 4) Double-buffered prefetch stress
    {
        BlockHandle h = eng.allocate(PoolType::ModelWeights, 4<<20, 256, "double_buf");
        assert(h);
        for (int i=0;i<5;i++) {
            eng.offload_to_ddr(h, nullptr, true);  // keep VRAM copy
            eng.prefetch_to_vram(h, nullptr, true);
        }
        eng.deallocate(h);
        printf("[test] double-buffer stress OK\n");
    }

    eng.print_pool_stats();

    for (auto h : handles) eng.deallocate(h);
    eng.synchronize_all_streams();
    eng.print_pool_stats();

    eng.shutdown();
    printf("[test] shutdown OK — PASS\n");
    return 0;
}
