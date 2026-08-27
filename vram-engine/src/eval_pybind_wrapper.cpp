/**
 * eval_pybind_wrapper.cpp — PyTorch C++ Extension Binding for red-eval
 * ======================================================================
 * Exposes the Red Daft Isolated Evaluation Engine to Python so it runs
 * seamlessly inside Kaggle / Google Colab benchmark environments.
 *
 *   import red_eval
 *   red_eval.set_backend_mock()
 *   red_eval.run_diagnostics()
 *   print(red_eval.report_json())
 *   print(red_eval.export_failed_dataset("red_failed_dataset.jsonl"))
 *
 * Compiles to `red_eval*.so`. On CPU-only runners compile without RD_USE_* —
 * the engine falls back to the deterministic mock backend and the full
 * probe/matrix/exporter pipeline still runs in CI/Kaggle.
 */

#include "red_eval_engine.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

namespace py = pybind11;
using namespace red_daft;

// Optional Python callable as the probe model backend.
static void set_backend_callable(py::object fn) {
    ProbeModelBackend b;
    b.is_mock = false;
    b.run = [fn](const std::string& prompt, size_t max_tokens) {
        // Call python fn(prompt, max_tokens) -> (latency, tokens, output)
        py::gil_scoped_acquire gil;
        py::tuple t = fn(prompt, max_tokens);
        double lat = t[0].cast<double>();
        size_t toks = t[1].cast<size_t>();
        std::string out = t[2].cast<std::string>();
        return std::make_tuple(lat, toks, out);
    };
    eval_engine().set_backend(b);
}

static void set_backend_mock() {
    eval_engine().set_backend(ProbeModelBackend::mock());
}

static void set_skill_enabled(int skill, bool on) {
    eval_engine().set_skill_enabled(static_cast<EvalSkill>(skill), on);
}

static bool skill_enabled(int skill) {
    return eval_engine().skill_enabled(static_cast<EvalSkill>(skill));
}

PYBIND11_MODULE(red_eval, m) {
    m.doc() = R"pbdoc(
        Red Daft OS — Isolated Evaluation Engine (red-eval)
        ================================
        5 Core Vector diagnostic matrix:
          (a) GeneralLanguage  (b) ReasoningMath  (c) CodeGenSyntax
          (d) StructuredOutput (e) DomainKnowledge
        Runs lightweight async probes, computes a JSON diagnostic report,
        toggles skills (triggering LoRa hot-swaps), and exports failure
        cases to red_failed_dataset.jsonl for LoRa fine-tuning.

        Usage:
            import red_eval
            red_eval.set_backend_mock()          # deterministic (CI-safe)
            red_eval.set_skill_enabled(2, False) # disable CodeGenSyntax
            red_eval.run_diagnostics()
            print(red_eval.report_json())
            n = red_eval.export_failed_dataset("red_failed_dataset.jsonl")
    )pbdoc";

    m.def("set_backend_mock", &set_backend_mock,
          "Use the deterministic mock backend (safe on CI/Kaggle/Colab).");
    m.def("set_backend_callable", &set_backend_callable, py::arg("fn"),
          "Use a Python callable fn(prompt, max_tokens) -> (latency, tokens, output).");
    m.def("run_diagnostics", []() {
        eval_engine().run_diagnostics_async();
        eval_engine().await_completion(30000);
    }, "Run all enabled-skill probes synchronously (waits for completion).");
    m.def("report_json", []() { return eval_engine().report_json(); },
          "Run all probes and return the JSON diagnostic report.");
    m.def("report", []() { return eval_engine().report(); },
          "Return the DiagnosticReport (skills, totals).");
    m.def("export_failed_dataset", [](const std::string& path) {
              return eval_engine().export_failed_dataset(
                  path.empty() ? "red_failed_dataset.jsonl" : path);
          }, py::arg("path"),
          "Export captured failures to the given JSONL path; returns count.");
    m.def("set_skill_enabled", &set_skill_enabled, py::arg("skill"), py::arg("on"),
          "Toggle a skill (0-4). Fast setter that also triggers LoRa hot-swap.");
    m.def("skill_enabled", &skill_enabled, py::arg("skill"),
          "Query whether a skill (0-4) is enabled.");
    m.def("active_pools", []() { return eval_engine().is_busy() ? 1 : 0; },
          "Whether evaluation is busy (in-flight probes).");
    m.def("peak_ram_mb", []() { return eval_engine().peak_ram_mb(); },
          "Peak RAM observed by the engine (MB).");

    py::class_<SkillScore>(m, "SkillScore")
        .def_readonly("skill", &SkillScore::skill)
        .def_readonly("overall", &SkillScore::overall)
        .def_readonly("accuracy", &SkillScore::accuracy)
        .def_readonly("latency_us", &SkillScore::latency_us)
        .def_readonly("probes_run", &SkillScore::probes_run)
        .def_readonly("probes_passed", &SkillScore::probes_passed)
        .def_readonly("enabled", &SkillScore::enabled);
}
