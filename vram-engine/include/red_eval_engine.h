#pragma once
/**
 * red_eval_engine.h — Red Daft OS Isolated Evaluation Engine (`red-eval`)
 * =======================================================================
 * Lightweight, asynchronous LLM diagnostic & skill-profiling engine.
 *
 * Design goals (spec):
 *  1. ISOLATED & LOW MEMORY: consumes < ~100 MB RAM, runs probe evaluations
 *     asynchronously WITHOUT altering the states of active VRAM Pools 0-9.
 *     Uses lightweight perplexity probes + targeted multi-shot prompts to
 *     assess latency, reasoning accuracy, code syntax validity, and
 *     structured-output (JSON/Tool) adherence.
 *  2. DIAGNOSTIC MATRIX & SKILL PROFILING: a scoring matrix over 5 Core
 *     Vectors (a..e), emitted as a structured JSON diagnostic report.
 *  3. INTERACTIVE CLI & AUTO-TUNING: `red-eval --tune` lets users toggle
 *     skills on/off. Selections drive microsecond LoRa hot-swapping into the
 *     Red Daft AI Kernel via a zero-copy IPC backend (`RedCtlIpc`), which
 *     falls back to in-process `LoRaSwapper` calls on Linux/CPU.
 *  4. FINE-TUNING DATASET EXPORTER: observed failure cases (hallucinations,
 *     syntax errors) are dumped to `red_failed_dataset.jsonl` for targeted
 *     LoRa fine-tuning.
 *
 * Zero-copy IPC: the engine communicates with the AI-Kernel's `redctl`
 * through a small Unix Domain Socket protocol (see RedCtlIpc). When no live
 * `redctl` is reachable (Kaggle/Colab/CPU), the same calls route to the local
 * LoRaSwapper so the pipeline still exercises the hot-swap path.
 *
 * Code organization:
 *   - Diagnostic Logic  -> RedEvalEngine (this file + red_eval_engine.cpp)
 *   - UI Rendering      -> red_eval_cli.cpp (separate translation unit)
 *   - System Kernel IPC -> RedCtlIpc (zero-copy socket client)
 *
 * c++20, POSIX threads (std::thread / std::async). License: Red Daft OS
 * Verbatim Distribution License v1.0 (see /LICENSE).
 */

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "red_daft_lora_manager.h"

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// 5 Core Vectors (Diagnostic Matrix) + LoRa pool mapping
// ─────────────────────────────────────────────────────────────────────
enum class EvalSkill : uint8_t {
    GeneralLanguage   = 0, // (a) Context comprehension
    ReasoningMath     = 1, // (b) Complex reasoning & math logic
    CodeGenSyntax     = 2, // (c) Code generation & syntax validity
    StructuredOutput  = 3, // (d) JSON / function-calling adherence
    DomainKnowledge   = 4, // (e) Cybersecurity / Legal / Finance, etc.
    COUNT             = 5
};
constexpr size_t kEvalSkillCount = 5;

inline constexpr std::array<std::string_view, kEvalSkillCount> kEvalSkillNames = {
    "GeneralLanguage", "ReasoningMath", "CodeGenSyntax",
    "StructuredOutput", "DomainKnowledge"
};
inline std::string_view to_string(EvalSkill s) {
    return kEvalSkillNames[static_cast<size_t>(s)];
}

// Map this engine's 5 skills to the LoRa Brain Engine's 4 pools.
// DomainKnowledge is assigned to the SystemControl pool (cross-cutting).
inline std::optional<LoRaPool> skill_to_lora_pool(EvalSkill s) {
    switch (s) {
        case EvalSkill::GeneralLanguage:  return LoRaPool::ConversationLang;
        case EvalSkill::ReasoningMath:    return LoRaPool::ReasoningLogic;
        case EvalSkill::CodeGenSyntax:    return LoRaPool::CodingSyntax;
        case EvalSkill::StructuredOutput: return LoRaPool::SystemControl;
        case EvalSkill::DomainKnowledge:  return LoRaPool::SystemControl;
        default: return std::nullopt;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Probe types
// ─────────────────────────────────────────────────────────────────────
enum class EvalProbeType : uint8_t {
    Perplexity  = 0,  // lightweight log-perplexity over a token window
    MultipleChoice=1, // targeted multi-shot reasoning question
    CodeGen     = 2,  // syntactic validity of generated code
    JsonSchema  = 3,  // structured output / function-call adherence
    Domain      = 4   // domain knowledge factoid
};

// ─────────────────────────────────────────────────────────────────────
// Result structures
// ─────────────────────────────────────────────────────────────────────
struct ProbeResult {
    EvalProbeType type = EvalProbeType::Perplexity;
    EvalSkill     skill = EvalSkill::GeneralLanguage;
    uint32_t      id   = 0;
    double        score = 0.0;       // normalized 0..1 (1 = perfect)
    double        latency_us = 0.0;
    size_t        tokens = 0;
    bool          passed = true;
    bool          failed_was_hallucination = false; // flagged by a verifier
    std::string   prompt;
    std::string   model_output;
    std::string   expected;          // verifier ground truth (if any)
    std::string   error;             // subset of model_output flagged as wrong
};

struct SkillScore {
    EvalSkill     skill = EvalSkill::GeneralLanguage;
    double        overall = 0.0;     // weighted mean of probes 0..1
    double        latency_us = 0.0;  // avg latency
    double        accuracy = 0.0;    // fraction passed
    size_t        probes_run = 0;
    size_t        probes_passed = 0;
    bool          enabled = true;    // user-toggleable in --tune
};

struct DiagnosticReport {
    std::vector<SkillScore> skills;
    size_t total_probes = 0;
    size_t total_passed = 0;
    size_t total_failed = 0;
    double peak_ram_mb = 0.0;
    double total_time_us = 0.0;
    uint64_t timestamp_unix = 0;

    // serialized JSON report
    std::string to_json() const;
};

// One failure captured for LoRa fine-tuning
struct FailedInstance {
    uint32_t probe_id = 0;
    EvalSkill skill = EvalSkill::GeneralLanguage;
    EvalProbeType type = EvalProbeType::Perplexity;
    std::string prompt;
    std::string model_output;
    std::string expected;
    std::string error;
    double score = 0.0;
    bool hallucination = false;
    int64_t timestamp_unix = 0;

    std::string to_jsonl() const;
};

// ─────────────────────────────────────────────────────────────────────
// RedCtlIpc — zero-copy IPC to the AI-Kernel (`redctl`)
// ─────────────────────────────────────────────────────────────────────
// Protocol (UDS datagram, little-endian explicit framing):
//   REDCTL\0<op:u8><payload size:u32><payload>
//   op 1 = swap_lora (payload: skill-id:u8, on:u8)
//   ack  = 1 byte: 0 = no redctl (fall back to local), 1 = accepted
class RedCtlIpc {
public:
    RedCtlIpc();
    ~RedCtlIpc();

    RedCtlIpc(const RedCtlIpc&) = delete;
    RedCtlIpc& operator=(const RedCtlIpc&) = delete;

    // Connect lazily; returns true if a live redctl socket exists.
    bool connect_to_redctl(std::string_view sock_path = "/tmp/redctl.sock");
    bool is_connected() const { return fd_ >= 0; }

    // Ask redctl to hot-swap a LoRa into the AI-Kernel for a given skill.
    // Returns true if redctl accepted (kernel handled it), false otherwise.
    bool request_skill_lora(EvalSkill skill, bool on);

    // In-process fallback: direct microsecond LoRa hot-swap via LoRaSwapper.
    // No redctl needed — used on Kaggle/Colab/CPU and as the primary Linux path.
    bool swap_local(EvalSkill skill, bool on);

    // Combine: try redctl first, else route locally. Returns true if handled.
    bool dispatch(EvalSkill skill, bool on);

private:
    int fd_ = -1;
    std::string sock_path_;
};

// ─────────────────────────────────────────────────────────────────────
// Lightweight model backend — pluggable probe executor
// ─────────────────────────────────────────────────────────────────────
// `ProbeModelBackend` is how `red-eval` talks to the underlying LLM
// (Ollama / OpenAI-compatible / redctl / mock). In CI/CPU fallback we use
// a deterministic mock so the pipeline, matrix, and exporter are testable.
struct ProbeModelBackend {
    // Callback: run a probe prompt, return (latency_us, tokens, output_text).
    using RunFn = std::function<std::tuple<double, size_t, std::string>(
                        const std::string& prompt, size_t max_tokens)>;
    RunFn run = nullptr;
    bool  is_mock = false;

    static ProbeModelBackend mock();                       // deterministic
    static ProbeModelBackend ollama(const std::string& base_url = "http://127.0.0.1:11434");
};

// ─────────────────────────────────────────────────────────────────────
// RedEvalEngine — core diagnostic orchestrator
// ─────────────────────────────────────────────────────────────────────
class RedEvalEngine {
public:
    RedEvalEngine();
    ~RedEvalEngine();
    RedEvalEngine(const RedEvalEngine&) = delete;
    RedEvalEngine& operator=(const RedEvalEngine&) = delete;

    // ── Configuration ─────────────────────────────────────────────
    void set_backend(ProbeModelBackend backend);
    void set_ram_budget_mb(double mb);              // guard < 100 MB target
    void set_ipc(RedCtlIpc* ipc);                   // null -> auto local-only

    // ── Skill toggles (used by --tune) ────────────────────────────
    void set_skill_enabled(EvalSkill s, bool on);
    bool skill_enabled(EvalSkill s) const;

    // ── Probing ───────────────────────────────────────────────────
    // Run one asynchronous probe of a given type. Does not block caller.
    void probe_async(EvalSkill skill, EvalProbeType type,
                     std::string prompt, std::string expected = {});

    // Run all enabled skills with representative probes (async batch)
    void run_diagnostics_async();

    // ── Results ───────────────────────────────────────────────────
    // Wait for in-flight probes and fold results into skill scores.
    void await_completion(int timeout_ms = 60000);

    // Produce the JSON diagnostic report.
    DiagnosticReport report() const;
    std::string report_json() const { return report().to_json(); }

    // ── Failure exporter ──────────────────────────────────────────
    // Export captured failures to red_failed_dataset.jsonl (for LoRa tuning)
    size_t export_failed_dataset(const std::string& path = "red_failed_dataset.jsonl");

    const std::vector<FailedInstance>& failed_instances() const { return failures_; }
    bool is_busy() const { return pending_ > 0; }
    double peak_ram_mb() const { return peak_ram_mb_.load(); }

private:
    // Scoring matrix helpers
    static double score_json(std::string_view out, std::string_view expected);
    static double score_perplexity(size_t tokens, double latency_us);
    static double score_code_syntax(std::string_view out);
    static double score_mc(std::string_view out, std::string_view expected);

    void flush_probe(const ProbeResult& r);

    mutable std::shared_mutex mtx_;
    ProbeModelBackend backend_;
    RedCtlIpc* ipc_ = nullptr;   // null => use local_ipc_
    RedCtlIpc local_ipc_;
    std::array<bool, kEvalSkillCount> enabled_;
    std::array<std::vector<ProbeResult>, kEvalSkillCount> probes_;
    std::array<SkillScore, kEvalSkillCount> scores_;

    std::vector<FailedInstance> failures_;
    mutable std::mutex failures_mtx_;
    std::atomic<uint32_t> probe_id_{1};
    std::atomic<uint32_t> pending_{0};
    std::atomic<double> peak_ram_mb_{0.0};
    std::condition_variable idle_cv_;
    std::mutex idle_mtx_;
    double ram_budget_mb_ = 100.0;
};

// ─────────────────────────────────────────────────────────────────────
// RedEvalEngine free-function convenience wrapper (shares singleton)
// ─────────────────────────────────────────────────────────────────────
RedEvalEngine& eval_engine();

} // namespace red_daft
