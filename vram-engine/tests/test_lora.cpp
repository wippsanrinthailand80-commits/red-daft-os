/**
 * test_lora.cpp — Native C++ smoke test for Red Daft LoRa Brain Engine
 * Build: g++ -std=c++20 -I../include ../src/red_daft_vram.cpp
 *        ../src/red_daft_lora_manager.cpp test_lora.cpp -o lora_test
 * Or via CMake: make lora_test && ./lora_test
 *
 * Tests:
 *  1. Register 4 LoRa adapters (system/reasoning/coding/conversation)
 *  2. Hot-swap between adapters (async, verify VRAM state)
 *  3. LRU eviction under VRAM pressure
 *  4. SGMV weight patching (CPU fallback path)
 *  5. Named pool swap convenience methods
 *  6. Registry stats and LRU tracking
 */
#include "red_daft_lora_manager.h"
#include "red_daft_vram.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace red_daft;

// Generate dummy LoRA weights (simple pattern for verification)
static void fill_pattern(float* data, size_t count, uint32_t seed) {
    for (size_t i = 0; i < count; ++i) {
        data[i] = static_cast<float>((seed * 1337 + i * 7 + 42) % 1000) / 1000.0f - 0.5f;
    }
}

// Verify two buffers match
static bool buffers_match(const float* a, const float* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
    }
    return true;
}

int main() {
    printf("=== Red Daft LoRa Brain Engine — C++ Smoke ===\n");

    // ── Initialize VRAM Engine (CPU fallback) ──────────────────────
    EngineConfig ecfg;
    ecfg.vram_budget_bytes = 256ULL << 20;  // 256 MiB for testing
    ecfg.host_budget_bytes = 512ULL << 20;
    ecfg.num_streams = 4;

    auto& vram = VramEngine::instance();
    if (!vram.is_initialized()) {
        assert(vram.initialize(ecfg));
    }
    printf("[test] VRAM engine initialized (256 MiB budget)\n");

    // ── Create Registry + Swapper ──────────────────────────────────
    LoRaRegistry registry;
    LoRaSwapper swapper(registry, vram);
    printf("[test] Registry + Swapper created\n");

    // ── Test 1: Register 4 LoRa adapters ──────────────────────────
    printf("\n--- Test 1: Register adapters ---\n");

    const uint32_t rank = 16;
    const uint32_t in_feat = 256;   // small for testing
    const uint32_t out_feat = 256;
    const size_t a_elems = (size_t)rank * in_feat;
    const size_t b_elems = (size_t)out_feat * rank;
    const size_t a_bytes = a_elems * sizeof(float);
    const size_t b_bytes = b_elems * sizeof(float);

    std::vector<float> buf_A(a_elems);
    std::vector<float> buf_B(b_elems);

    // System LoRa (pool 1)
    fill_pattern(buf_A.data(), a_elems, 1);
    fill_pattern(buf_B.data(), b_elems, 2);
    uint32_t id_sys = registry.register_adapter(
        "system_agent", LoRaPool::SystemControl, rank, in_feat, out_feat,
        buf_A.data(), buf_B.data());
    assert(id_sys != 0);
    printf("[test] Registered 'system_agent' id=%u pool=SystemControl\n", id_sys);

    // Reasoning LoRa (pool 2)
    fill_pattern(buf_A.data(), a_elems, 3);
    fill_pattern(buf_B.data(), b_elems, 4);
    uint32_t id_reas = registry.register_adapter(
        "reasoning_v2", LoRaPool::ReasoningLogic, rank, in_feat, out_feat,
        buf_A.data(), buf_B.data());
    assert(id_reas != 0);
    printf("[test] Registered 'reasoning_v2' id=%u pool=ReasoningLogic\n", id_reas);

    // Coding LoRa (pool 3)
    fill_pattern(buf_A.data(), a_elems, 5);
    fill_pattern(buf_B.data(), b_elems, 6);
    uint32_t id_code = registry.register_adapter(
        "coding_expert", LoRaPool::CodingSyntax, rank, in_feat, out_feat,
        buf_A.data(), buf_B.data());
    assert(id_code != 0);
    printf("[test] Registered 'coding_expert' id=%u pool=CodingSyntax\n", id_code);

    // Conversation LoRa (pool 4)
    fill_pattern(buf_A.data(), a_elems, 7);
    fill_pattern(buf_B.data(), b_elems, 8);
    uint32_t id_conv = registry.register_adapter(
        "chat_enhanced", LoRaPool::ConversationLang, rank, in_feat, out_feat,
        buf_A.data(), buf_B.data());
    assert(id_conv != 0);
    printf("[test] Registered 'chat_enhanced' id=%u pool=ConversationLang\n", id_conv);

    assert(registry.count() == 4);
    printf("[test] Registry has %zu adapters OK\n", registry.count());

    // ── Test 2: Hot-swap between adapters ─────────────────────────
    printf("\n--- Test 2: Hot-swap sequence ---\n");

    // Swap to system LoRa
    assert(swapper.swap_to_system_lora());
    swapper.synchronize();
    assert(swapper.active_adapter_id() == id_sys);
    assert(swapper.is_in_vram(id_sys));
    printf("[test] Swap to system OK (id=%u)\n", id_sys);

    // Swap to coding LoRa (should evict system)
    assert(swapper.swap_to_coding_lora());
    swapper.synchronize();
    assert(swapper.active_adapter_id() == id_code);
    assert(swapper.is_in_vram(id_code));
    printf("[test] Swap to coding OK (id=%u)\n", id_code);

    // Swap to reasoning LoRa
    assert(swapper.swap_to_reasoning_lora());
    swapper.synchronize();
    assert(swapper.active_adapter_id() == id_reas);
    printf("[test] Swap to reasoning OK (id=%u)\n", id_reas);

    // Swap to conversation LoRa
    assert(swapper.swap_to_conversation_lora());
    swapper.synchronize();
    assert(swapper.active_adapter_id() == id_conv);
    printf("[test] Swap to conversation OK (id=%u)\n", id_conv);

    // ── Test 3: LRU eviction ─────────────────────────────────────
    printf("\n--- Test 3: LRU eviction ---\n");

    // Touch adapters in order to set up LRU
    registry.touch(id_sys);
    registry.touch(id_reas);
    registry.touch(id_code);
    registry.touch(id_conv);

    // The LRU victim should be system_agent (touched first)
    LoRaAdapter* victim = registry.lru_victim();
    assert(victim != nullptr);
    assert(victim->id == id_sys);
    printf("[test] LRU victim is '%s' id=%u OK\n", victim->name.c_str(), victim->id);

    // ── Test 4: SGMV weight patching ─────────────────────────────
    printf("\n--- Test 4: SGMV weight patching ---\n");

    // Re-swap to coding LoRa (it may have been evicted by swaps)
    assert(swapper.hot_swap_async(id_code));
    swapper.synchronize();

    // Generate input and output buffers
    std::vector<float> input(in_feat);
    std::vector<float> output(out_feat, 0.0f);
    fill_pattern(input.data(), in_feat, 99);

    // Apply LoRA weights (SGMV)
    assert(swapper.apply_lora_weights(id_code, output.data(), input.data(), 1));
    printf("[test] SGMV apply on 'coding_expert' OK\n");

    // Verify output is non-zero (SGMV actually computed something)
    float sum = 0.0f;
    for (size_t i = 0; i < out_feat; ++i) sum += std::fabs(output[i]);
    assert(sum > 0.0f);
    printf("[test] SGMV output non-zero (sum=%.4f) OK\n", sum);

    // ── Test 5: Register empty adapter + verify host data ─────────
    printf("\n--- Test 5: Register empty + verify host data ---\n");

    uint32_t id_empty = registry.register_empty(
        "empty_lora", LoRaPool::CodingSyntax, rank, in_feat, out_feat);
    assert(id_empty != 0);

    LoRaAdapter* empty_a = registry.find_by_id(id_empty);
    assert(empty_a != nullptr);
    assert(empty_a->h_A != nullptr);
    assert(empty_a->h_B != nullptr);
    // Empty adapter should have zero-initialized host data
    float host_sum = 0.0f;
    const float* ha = static_cast<const float*>(empty_a->h_A);
    for (size_t i = 0; i < a_elems; ++i) host_sum += std::fabs(ha[i]);
    assert(host_sum == 0.0f);
    printf("[test] Empty adapter host data zeroed OK\n");

    // ── Test 6: Verify host data integrity after swap ─────────────
    printf("\n--- Test 6: Host data integrity ---\n");

    // Re-register with known data
    std::vector<float> verify_A(a_elems);
    std::vector<float> verify_B(b_elems);
    fill_pattern(verify_A.data(), a_elems, 42);
    fill_pattern(verify_B.data(), b_elems, 84);

    uint32_t id_verify = registry.register_adapter(
        "verify_lora", LoRaPool::ReasoningLogic, rank, in_feat, out_feat,
        verify_A.data(), verify_B.data());
    assert(id_verify != 0);

    LoRaAdapter* va = registry.find_by_id(id_verify);
    assert(va != nullptr);
    assert(buffers_match(static_cast<const float*>(va->h_A), verify_A.data(), a_elems));
    assert(buffers_match(static_cast<const float*>(va->h_B), verify_B.data(), b_elems));
    printf("[test] Host data matches original after registration OK\n");

    // ── Test 7: Swap stats ───────────────────────────────────────
    printf("\n--- Test 7: Stats ---\n");
    swapper.print_stats();
    auto s = swapper.stats();
    assert(s.total_swaps > 0);
    assert(s.total_bytes_transferred > 0);
    printf("[test] Stats: %llu swaps, %llu KB transferred, %.1f us avg\n",
           (unsigned long long)s.total_swaps,
           (unsigned long long)(s.total_bytes_transferred >> 10),
           s.avg_swap_time_us);

    // ── Test 8: Evict all + verify clean state ───────────────────
    printf("\n--- Test 8: Evict all ---\n");
    size_t evicted = swapper.evict_all_except_active();
    swapper.synchronize();
    printf("[test] Evicted %zu adapters\n", evicted);

    // Only active adapter should be in VRAM
    for (auto* a : registry.all_adapters()) {
        if (a->id == swapper.active_adapter_id()) {
            // Active adapter should still be in VRAM
            assert(a->is_on_vram());
        } else {
            // Others should be evicted
            assert(!a->is_on_vram());
        }
    }
    printf("[test] Evict-all verification OK\n");

    // ── Test 9: Register via raw bytes ────────────────────────────
    printf("\n--- Test 9: Register raw bytes ---\n");
    uint32_t id_raw = registry.register_adapter_raw(
        "raw_lora", LoRaPool::ConversationLang, rank, in_feat, out_feat,
        verify_A.data(), a_bytes, verify_B.data(), b_bytes);
    assert(id_raw != 0);
    printf("[test] Raw registration OK (id=%u)\n", id_raw);

    // ── Cleanup ───────────────────────────────────────────────────
    printf("\n--- Cleanup ---\n");
    registry.unregister(id_sys);
    registry.unregister(id_reas);
    registry.unregister(id_code);
    registry.unregister(id_conv);
    registry.unregister(id_empty);
    registry.unregister(id_verify);
    registry.unregister(id_raw);
    printf("[test] All adapters unregistered\n");

    vram.shutdown();
    printf("[test] VRAM engine shutdown\n");

    printf("\n=== LoRa Brain Engine — ALL TESTS PASS ===\n");
    return 0;
}
