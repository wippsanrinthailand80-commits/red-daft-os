/**
 * red_burst_cli.cpp — Red Daft OS `redctl lora load --mode ultra` CLI
 * ======================================================================
 * Entry point for the ultra-burst LoRA load command. Spins up the
 * RedBurstEngine with a 3-second (configurable) compute spike, quantizes
 * active LoRA adapters + KV cache into INT4/INT2/FP4 micro-buffers,
 * routes them into the 10-memory-pool system, and prints a JSON report.
 *
 * Usage:
 *   red_burst_cli lora load --mode ultra [--duration 3.0] [--quant int4] [--json]
 *   red_burst_cli lora load --mode ultra --help
 *
 * c++20, POSIX. License: Red Daft OS Verbatim Distribution License v1.0.
 */

#include "red_burst_engine.h"
#include "red_daft_vram.h"
#include "red_daft_lora_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

using namespace red_daft;

namespace {

void print_help() {
    std::printf(R"(
Usage: red_burst_cli lora load --mode ultra [options]

Ultra-burst LoRA load: 3-second GPU spike to quantize adapters + KV cache
into micro-buffers (INT4/INT2/FP4) and route to the 10-pool VRAM system.

Options:
  --duration SEC       Burst duration in seconds (default: 3.0, max: 60.0)
  --quant FORMAT       Quantization: int4, int2, fp4 (default: int4)
  --kv-quant FORMAT    KV cache quantization (default: int4)
  --device ID          GPU device ID (default: 0)
  --temp-limit C       Thermal throttle limit Celsius (default: 85)
  --json               Output JSON metrics only
  --help               Show this help

Examples:
  red_burst_cli lora load --mode ultra
  red_burst_cli lora load --mode ultra --duration 5.0 --quant int2 --json
)");
}

int run_ultra_burst(int argc, char** argv) {
    // defaults
    double duration = 3.0;
    BurstQuantType lora_q = BurstQuantType::Int4;
    BurstQuantType kv_q = BurstQuantType::Int4;
    int device_id = 0;
    float temp_limit = 85.0f;
    bool json_only = false;

    // parse
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        else if (arg == "--json") { json_only = true; }
        else if (arg == "--duration" && i + 1 < argc) { duration = std::stod(argv[++i]); }
        else if (arg == "--quant" && i + 1 < argc) {
            std::string q = argv[++i];
            if (q == "int4") lora_q = BurstQuantType::Int4;
            else if (q == "int2") lora_q = BurstQuantType::Int2;
            else if (q == "fp4") lora_q = BurstQuantType::Fp4;
            else { std::fprintf(stderr, "unknown quant: %s\n", q.c_str()); return 1; }
        }
        else if (arg == "--kv-quant" && i + 1 < argc) {
            std::string q = argv[++i];
            if (q == "int4") kv_q = BurstQuantType::Int4;
            else if (q == "int2") kv_q = BurstQuantType::Int2;
            else if (q == "fp4") kv_q = BurstQuantType::Fp4;
            else { std::fprintf(stderr, "unknown kv-quant: %s\n", q.c_str()); return 1; }
        }
        else if (arg == "--device" && i + 1 < argc) { device_id = std::stoi(argv[++i]); }
        else if (arg == "--temp-limit" && i + 1 < argc) { temp_limit = std::stof(argv[++i]); }
        else { std::fprintf(stderr, "unknown option: %s\n", arg.data()); return 1; }
    }

    // clamp
    if (duration <= 0 || duration > 60) { std::fprintf(stderr, "duration out of range (0,60]\n"); return 1; }

    // Create engine and configure
    RedBurstEngine eng;
    BurstConfig cfg;
    cfg.burst_seconds = duration;
    cfg.device_id = device_id;
    cfg.lora_quant = lora_q;
    cfg.kv_quant = kv_q;
    cfg.throttle_temp_c = temp_limit;
    if (!eng.configure(cfg)) {
        std::fprintf(stderr, "failed to configure burst engine\n");
        return 1;
    }

    // Hook into LoRa registry + pool system
    static LoRaRegistry reg;
    eng.set_lora_find_fn([&](uint32_t id) -> LoRaAdapter* { return reg.find_by_id(id); });

    std::array<size_t, 10> pool_bytes{};
    eng.set_pool_push_fn([&](PoolType p, const void*, size_t bytes) {
        pool_bytes[static_cast<size_t>(p)] += bytes;
    });

    // Submit a representative KV workload (in production this comes from NanoContext)
    BurstKvSource kv;
    static float kvdata[4096];
    for (int i = 0; i < 4096; ++i) kvdata[i] = (float)(i % 5) - 2.f;
    kv.data = kvdata;
    kv.elements = 4096;
    kv.head_dim = 64;
    kv.dest_pool = PoolType::KvCache;
    eng.submit_kv(kv);

    // Note: In full integration, all active LoRA adapters from registry are auto-submitted.
    // For CLI demo, we submit none (registry empty) - burst will just quant KV.

    // Execute
    BurstResult res = eng.execute_ultra_burst();

    if (!json_only) {
        std::printf("\n"
            "┌────────────────────────────────────────────────────────────┐\n"
            "│  Red-burst Ultra-LoRA Engine — Burst Complete             │\n"
            "└────────────────────────────────────────────────────────────┘\n");
        std::printf("  Duration:     %.3f s (target %.3f s)\n", res.metrics.peak_elapsed_s, duration);
        std::printf("  GPU peak:     %.1f%%\n", res.metrics.gpu_percent);
        std::printf("  Watchdog:     %s\n", res.metrics.watchdog_engaged ? "ENGAGED" : "idle");
        std::printf("  Throttled:    %s\n", res.metrics.throttled ? "yes" : "no");
        std::printf("  Quant ops:    %zu\n", res.metrics.quant_ops);
        std::printf("  Bytes in:     %zu\n", res.metrics.combined_bytes_in);
        std::printf("  Bytes out:    %zu (%.2fx compression)\n",
                    res.metrics.combined_bytes_out,
                    res.metrics.combined_bytes_in ? (double)res.metrics.combined_bytes_in / res.metrics.combined_bytes_out : 0);
        std::printf("  Pools hit:    ");
        bool first = true;
        for (size_t i = 0; i < pool_bytes.size(); ++i) {
            if (pool_bytes[i] > 0) {
                if (!first) std::printf(", ");
                std::printf("%s=%zu", (i < 10 ? "P" : "?"), pool_bytes[i]);
                first = false;
            }
        }
        if (first) std::printf("(none)");
        std::printf("\n");
    } else {
        // JSON output
        std::printf("{\n");
        std::printf("  \"ok\": %s,\n", res.ok ? "true" : "false");
        std::printf("  \"duration_s\": %.6f,\n", res.metrics.peak_elapsed_s);
        std::printf("  \"target_duration_s\": %.3f,\n", duration);
        std::printf("  \"gpu_percent\": %.2f,\n", res.metrics.gpu_percent);
        std::printf("  \"watchdog_engaged\": %s,\n", res.metrics.watchdog_engaged ? "true" : "false");
        std::printf("  \"throttled\": %s,\n", res.metrics.throttled ? "true" : "false");
        std::printf("  \"quant_ops\": %zu,\n", res.metrics.quant_ops);
        std::printf("  \"bytes_in\": %zu,\n", res.metrics.combined_bytes_in);
        std::printf("  \"bytes_out\": %zu,\n", res.metrics.combined_bytes_out);
        std::printf("  \"pool_bytes\": [");
        for (size_t i = 0; i < pool_bytes.size(); ++i) {
            if (i) std::printf(", ");
            std::printf("%zu", pool_bytes[i]);
        }
        std::printf("]\n");
        std::printf("}\n");
    }

    return res.ok ? 0 : 1;
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc < 5) { print_help(); return 1; }
    std::string_view a1 = argv[1];
    std::string_view a2 = argv[2];
    std::string_view a3 = argv[3];
    std::string_view a4 = argv[4];

    if (a1 == "lora" && a2 == "load" && a3 == "--mode" && a4 == "ultra") {
        // shift argv so run_ultra_burst sees options starting at index 1
        return run_ultra_burst(argc - 5, argv + 5);
    }
    std::fprintf(stderr, "usage: red_burst_cli lora load --mode ultra [options]\n");
    return 1;
}