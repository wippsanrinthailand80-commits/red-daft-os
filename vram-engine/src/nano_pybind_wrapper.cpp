/**
 * nano_pybind_wrapper.cpp — PyTorch C++ Extension Binding for the
 * Red Daft OS Nano-Context Engine
 * ============================================================================
 * Exposes the ephemeral, high-density Nano-Pools (Input/Output Context Window,
 * KV Cache with FP16 -> INT4/INT2 quantization, token stream ring) to Python.
 *
 *   import red_daft_nano_context
 *   nano.nano_initialize(cfg)
 *   rid = nano.request_begin()
 *   nano.kv_store(rid, layer=0, head=0, key=key, value=val, seq_tokens=1)
 *   nano.stream_push(rid, [1.0, 2.0, 3.0])
 *   o = nano.stream_pop(rid, 3)
 *   nano.request_end(rid)
 *
 * Compile to `red_daft_nano_context*.so`. On CPU-only runners compile
 * without RD_USE_* — the engine falls back to malloc and the smoke test runs.
 */

#include "red_daft_nano_context.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <vector>

namespace py = pybind11;
using namespace red_daft;

// ── Free-function wrappers (avoid pybind member-pointer arg counting quirks)
static bool nano_init(const NanoContextConfig& cfg) {
    return NanoContextEngine::instance().initialize(cfg);
}

static uint64_t req_begin() { return NanoContextEngine::instance().request_begin(); }
static void req_end(uint64_t id) { NanoContextEngine::instance().request_end(id); }

static bool kv_store(uint64_t id, size_t layer, size_t head,
                     const std::vector<float>& key, const std::vector<float>& value,
                     size_t seq_tokens) {
    return NanoContextEngine::instance().kv_store(
        id, layer, head, key.data(), value.data(), seq_tokens);
}

static bool kv_load(uint64_t id, size_t layer, size_t head,
                    std::vector<float>& key_out, std::vector<float>& value_out,
                    size_t seq_tokens) {
    return NanoContextEngine::instance().kv_load(
        id, layer, head, key_out.data(), value_out.data(), seq_tokens);
}

static bool stream_push(uint64_t id, const std::vector<float>& tokens) {
    return NanoContextEngine::instance().stream_push(id, tokens.data(), tokens.size());
}

static std::vector<float> stream_pop(uint64_t id, size_t max_n) {
    std::vector<float> out(max_n, 0.f);
    NanoContextEngine::instance().stream_pop(id, out.data(), max_n);
    return out;
}

static NanoPoolStats pool_stats(uint64_t id) {
    return NanoContextEngine::instance().pool_stats(id);
}

static bool print_stats() {
    NanoContextEngine::instance().print_stats();
    return true;
}

PYBIND11_MODULE(red_daft_nano_context, m) {
    m.doc() = R"pbdoc(
        Red Daft OS Nano-Context Engine
        ================================
        Ephemeral, high-density context-window manager for LLM inference.

        Keeps Base Model Weights STRICTLY in FP16 in VRAM Pool 0 and routes
        ALL Input/Output Context Window + KV Cache allocations to dynamically
        spawned Nano-Pools with FP16 -> INT4/INT2 KV quantization and an
        ephemeral token stream ring (< 50 MB per active stream). Nano-Pools
        spawn on request start and recycle on generation completion.

        Usage:
            import red_daft_nano_context as nano
            cfg = nano.NanoContextConfig(kv_layers=8, kv_heads=4, head_dim=128)
            nano.nano_initialize(cfg)
            rid = nano.request_begin()
            key = [0.5]*128;  val = [0.25]*128
            nano.kv_store(rid, 0, 0, key, val, 1)
            nano.stream_push(rid, [1.0, 2.0, 3.0])
            o = nano.stream_pop(rid, 3)
            nano.request_end(rid)
    )pbdoc";

    py::class_<NanoContextConfig>(m, "NanoContextConfig")
        .def(py::init<>())
        .def_readwrite("kv_layers",          &NanoContextConfig::kv_layers)
        .def_readwrite("kv_heads",           &NanoContextConfig::kv_heads)
        .def_readwrite("head_dim",           &NanoContextConfig::head_dim)
        .def_readwrite("max_tokens",         &NanoContextConfig::max_tokens)
        .def_readwrite("stream_capacity",    &NanoContextConfig::stream_capacity)
        .def_readwrite("free_list_capacity", &NanoContextConfig::free_list_capacity)
        .def(py::init([](size_t kv_layers, size_t kv_heads, size_t head_dim,
                         size_t max_tokens, size_t stream_capacity,
                         size_t free_list_capacity) {
            NanoContextConfig c;
            c.kv_layers = kv_layers;
            c.kv_heads  = kv_heads;
            c.head_dim  = head_dim;
            c.max_tokens = max_tokens;
            c.stream_capacity = stream_capacity;
            c.free_list_capacity = free_list_capacity;
            return c;
        }), py::arg("kv_layers") = 32, py::arg("kv_heads") = 8,
            py::arg("head_dim") = 128, py::arg("max_tokens") = 4096,
            py::arg("stream_capacity") = (size_t)(24ULL << 20),
            py::arg("free_list_capacity") = 8);

    py::class_<NanoQuantStats>(m, "NanoQuantStats")
        .def_readonly("kv_entries",       &NanoQuantStats::kv_entries)
        .def_readonly("bytes_compressed", &NanoQuantStats::bytes_compressed)
        .def_readonly("bytes_saved",      &NanoQuantStats::bytes_saved)
        .def_readonly("quant_ops",        &NanoQuantStats::quant_ops);

    py::class_<NanoStreamStats>(m, "NanoStreamStats")
        .def_readonly("tokens_ingested", &NanoStreamStats::tokens_ingested)
        .def_readonly("tokens_emitted",  &NanoStreamStats::tokens_emitted)
        .def_readonly("overflows",       &NanoStreamStats::overflows);

    py::class_<NanoPoolStats>(m, "NanoPoolStats")
        .def_readonly("request_id",       &NanoPoolStats::request_id)
        .def_readonly("used_bytes",       &NanoPoolStats::used_bytes)
        .def_readonly("head_size",        &NanoPoolStats::head_size)
        .def_readonly("n_layers",         &NanoPoolStats::n_layers)
        .def_readonly("quant",            &NanoPoolStats::quant)
        .def_readonly("stream",           &NanoPoolStats::stream);

    // Lifecycle
    m.def("nano_initialize", &nano_init, py::arg("cfg") = NanoContextConfig{},
          "Initialize the Nano-Context engine with an optional config.");
    m.def("request_begin", &req_begin,
          "Spawn a fresh Nano-Pool and return its request id.");
    m.def("request_end", &req_end, py::arg("req_id"),
          "Recycle the Nano-Pool back to the free-list (generation complete).");

    // KV + stream ops
    m.def("kv_store", &kv_store,
          py::arg("req_id"), py::arg("layer"), py::arg("head"),
          py::arg("key"), py::arg("value"), py::arg("seq_tokens"),
          "Store a quantized KV cell for (layer, head) from flat float lists.");
    m.def("kv_load", &kv_load,
          py::arg("req_id"), py::arg("layer"), py::arg("head"),
          py::arg("key_out"), py::arg("value_out"), py::arg("seq_tokens"),
          "Dequantize a KV cell into pre-sized output lists.");
    m.def("stream_push", &stream_push, py::arg("req_id"), py::arg("tokens"),
          "Push tokens into the ephemeral token stream ring.");
    m.def("stream_pop", &stream_pop, py::arg("req_id"), py::arg("max_n"),
          "Pop up to max_n tokens from the stream ring (drains oldest).");

    // Introspection
    m.def("active_pools", []() { return NanoContextEngine::instance().active_pools(); },
          "Number of currently active Nano-Pools.");
    m.def("free_pools", []() { return NanoContextEngine::instance().free_pools(); },
          "Number of warm recycled Nano-Pools in the free-list.");
    m.def("pool_stats", &pool_stats, py::arg("req_id"),
          "Return NanoPoolStats for an active request.");
    m.def("print_stats", &print_stats,
          "Pretty-print engine statistics.");
}
