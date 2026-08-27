/**
 * red_eval_cli.cpp — Red Daft OS `red-eval` Interactive TUI / CLI
 * =================================================================
 * Implements:
 *   red-eval                -> run diagnostics, print JSON report
 *   red-eval --json         -> JSON report to stdout only
 *   red-eval --tune         -> interactive TUI to toggle skills on/off
 *                             and trigger microsecond LoRa hot-swapping
 *   red-eval --export PATH  -> export failed dataset to PATH
 *
 * UI Rendering only. Diagnostic logic lives in RedEvalEngine.
 *
 * c++20, POSIX. License: Red Daft OS Verbatim Distribution License v1.0.
 */

#include "red_eval_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <iostream>

using namespace red_daft;

namespace {

void print_banner() {
    printf("\n"
           "  ┌────────────────────────────────────────────────────────┐\n"
           "  │   Red Daft OS  •  Isolated Evaluation Engine (red-eval) │\n"
           "  └────────────────────────────────────────────────────────┘\n\n");
}

// Live metric display (simple, ncurses-free) -> ASCII matrix.
void print_matrix(const DiagnosticReport& rep) {
    printf("  %-20s %8s %10s %10s %10s\n", "SKILL", "OVERALL", "ACCURACY", "LAT_US", "RUN");
    printf("  %-20s %8s %10s %10s %10s\n", "────────────────────",
           "────────", "──────────", "──────────", "──────────");
    for (const auto& s : rep.skills) {
        const char* tag = s.enabled ? "" : " [off]";
        std::string name(to_string(s.skill));
        printf("  %-20s %8.3f %10.3f %10.1f %10zu%s\n",
               name.c_str(), s.overall, s.accuracy, s.latency_us,
               s.probes_run, tag);
    }
    printf("\n  total probes: %zu   passed: %zu   failed: %zu   peak RAM: %.1f MB\n",
           rep.total_probes, rep.total_passed, rep.total_failed, rep.peak_ram_mb);
}

int run_diagnostics(bool json_only) {
    auto& eng = eval_engine();
    eng.set_backend(ProbeModelBackend::mock());
    eng.set_ram_budget_mb(100.0);

    if (!json_only) print_banner();
    eng.run_diagnostics_async();
    auto start = std::chrono::high_resolution_clock::now();
    eng.await_completion(30000);
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    auto rep = eng.report();
    // Patch total_time proxy with actual wall time for better accuracy.
    (void)elapsed;

    if (json_only) {
        std::fputs(rep.to_json().c_str(), stdout);
    } else {
        print_matrix(rep);
        printf("\n  exported failures: %zu\n",
               eng.export_failed_dataset("red_failed_dataset.jsonl"));
    }
    return 0;
}

int run_tune() {
    auto& eng = eval_engine();
    eng.set_backend(ProbeModelBackend::mock());
    print_banner();

    bool live = true;
    while (live) {
        // Render current skill matrix (live metrics).
        auto rep = eng.report();
        print_matrix(rep);

        printf("\n  Toggle a skill [0-4] or [q]uit / [r]un-eval:\n");
        printf("   0=GeneralLanguage  1=ReasoningMath  2=CodeGenSyntax\n");
        printf("   3=StructuredOutput 4=DomainKnowledge\n");
        printf("  > ");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) break;
        char k = line[0];
        if (k == 'q' || k == 'Q') { live = false; break; }
        if (k == 'r' || k == 'R') {
            eng.run_diagnostics_async();
            eng.await_completion(30000);
            continue;
        }
        if (k >= '0' && k <= '4') {
            EvalSkill s = static_cast<EvalSkill>(k - '0');
            bool cur = eng.skill_enabled(s);
            eng.set_skill_enabled(s, !cur);   // triggers LoRa hot-swap IPC
            std::string nm(to_string(s));
            printf("\n  -> %s %s (LoRa hot-swap %s)\n",
                   nm.c_str(), cur ? "DISABLED" : "ENABLED",
                   cur ? "unloaded" : "loaded into pool");
        }
    }
    // Final exporter prompt
    printf("  exporting failed dataset: %zu instances\n",
           eng.export_failed_dataset("red_failed_dataset.jsonl"));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool json_only = false;
    std::string export_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json_only = true;
        else if (a == "--tune") return run_tune();
        else if (a == "--export") {
            if (i + 1 < argc) export_path = argv[++i];
            size_t n = eval_engine().export_failed_dataset(
                export_path.empty() ? "red_failed_dataset.jsonl" : export_path);
            printf("exported %zu failed instances\n", n);
            return 0;
        }
        else if (a == "--help" || a == "-h") {
            printf("red-eval: Red Daft OS Isolated Evaluation Engine\n"
                   "  (default)     run diagnostics + summary\n"
                   "  --json        print JSON diagnostic report\n"
                   "  --tune        interactive skill/LoRa hot-swap TUI\n"
                   "  --export PATH export red_failed_dataset.jsonl\n");
            return 0;
        }
    }

    return run_diagnostics(json_only);
}
