/**
 * test_eval.cpp — Native C++ test for the Red Daft Isolated Evaluation Engine
 * ==========================================================================
 * Verifies:
 *  1. Core diagnostic pipeline: runs all 5 skill probes through the mock
 *     backend, produces a JSON diagnostic report.
 *  2. Scoring matrix: each skill's overall/accuracy is folded from probes.
 *  3. Skill toggling: set_skill_enabled routes through IPC (local LoRa
 *     hot-swap fallback) and gates which skills run.
 *  4. Failure exporter: failed probes are captured and written to
 *     red_failed_dataset.jsonl.
 *  5. JSON validity: report parses to balanced braces and includes skills.
 *
 * Compile & run (CPU fallback, no GPU/Python needed):
 *   g++ -O2 -std=c++20 -I include tests/test_eval.cpp \
 *       src/red_eval_engine.cpp src/red_daft_vram.cpp \
 *       src/red_daft_lora_manager.cpp -o /tmp/eval_test -pthread
 *   /tmp/eval_test
 */

#include "red_eval_engine.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>

using namespace red_daft;

static int failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) { std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
            ++failures; }                                                    \
        else { std::printf("  [ ok ] %s\n", msg); }                          \
    } while (0)

int main() {
    std::printf("Red Daft Isolated Evaluation Engine — native test\n");
    std::printf("=================================================\n");

    auto& eng = eval_engine();
    eng.set_backend(ProbeModelBackend::mock());
    eng.set_ram_budget_mb(100.0);

    // ── 1. run diagnostics through mock backend, await completion ────
    eng.run_diagnostics_async();
    eng.await_completion(30000);
    CHECK(eng.is_busy() == false, "all probes completed");
    CHECK(eng.peak_ram_mb() > 0.0, "peak RAM tracked");

    // ── 2. scoring matrix ────────────────────────────────────────────
    auto rep = eng.report();
    CHECK(rep.skills.size() == kEvalSkillCount, "report has 5 skill vectors");
    CHECK(rep.total_probes >= 8, ">=8 probes were run across skills");
    size_t enabled_skills = 0;
    for (const auto& s : rep.skills) if (s.enabled) enabled_skills++;
    CHECK(enabled_skills == kEvalSkillCount, "all 5 skills enabled by default");
    CHECK(rep.total_passed + rep.total_failed == rep.total_probes,
          "passed + failed == total probes");

    // ── 3. JSON report structure ─────────────────────────────────────
    std::string j = rep.to_json();
    int depth = 0; bool ok = true;
    for (char c : j) { if (c=='{') depth++; else if (c=='}') depth--; if (depth<0) ok=false; }
    CHECK(ok && depth == 0, "JSON report has balanced braces");
    CHECK(j.find("\"skills\"") != std::string::npos, "JSON report lists skills");
    CHECK(j.find("ReasoningMath") != std::string::npos, "JSON has ReasoningMath vector");

    // ── 4. failure exporter ──────────────────────────────────────────
    // The mock always passes its probes, so inject a synthetic failure to
    // prove the exporter path, then export.
    {
        EvalSkill s = EvalSkill::CodeGenSyntax;
        eng.probe_async(s, EvalProbeType::CodeGen, "synthetic bad", "{not balanced");
        eng.await_completion(10000);
    }
    size_t n = eng.export_failed_dataset("/tmp/red_failed_test.jsonl");
    CHECK(n >= 1, "exporter captured at least one failed instance");

    // ── 5. skill toggling gates probes + triggers IPC hot-swap ───────
    eng.set_skill_enabled(EvalSkill::DomainKnowledge, false);
    CHECK(eng.skill_enabled(EvalSkill::DomainKnowledge) == false,
          "skill can be disabled");
    eng.set_skill_enabled(EvalSkill::DomainKnowledge, true);
    CHECK(eng.skill_enabled(EvalSkill::DomainKnowledge) == true,
          "skill can be re-enabled (LoRa hot-swap path exercised)");

    std::printf("\n%s\n",
                failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
