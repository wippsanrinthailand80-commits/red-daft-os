/**
 * lora_pybind_wrapper.cpp — PyTorch C++ Extension for LoRa Brain Engine
 * =====================================================================
 * Exposes the LoRa hot-swap engine to Python via pybind11.
 * Import and use:
 *
 *   import red_daft_lora as lora
 *
 *   # Register LoRa adapters
 *   lora.initialize()
 *   lora.register_lora("coding_v2", pool=3, rank=16, in_features=256, out_features=256)
 *   lora.register_lora("reasoning_v1", pool=2, rank=16, in_features=256, out_features=256)
 *
 *   # Hot-swap (microsecond-level)
 *   lora.swap_to_coding_lora()       # pool 3
 *   lora.swap_to_reasoning_lora()    # pool 2
 *   lora.swap_to_conversation_lora() # pool 4
 *   lora.swap_to_system_lora()       # pool 1
 *
 *   # Or by name
 *   lora.swap_to("coding_v2")
 *
 *   # Eviction / stats
 *   lora.evict_lru()
 *   lora.print_stats()
 *   lora.lora_stats()
 *
 * Build (NVIDIA CUDA):
 *   c++ -O3 -Wall -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \
 *       src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \
 *       src/lora_pybind_wrapper.cpp -o red_daft_lora$(python3-config --extension-suffix) \
 *       -DRD_USE_CUDA -lcudart
 *
 * Build (AMD ROCm):
 *   c++ -O3 -Wall -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \
 *       src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \
 *       src/lora_pybind_wrapper.cpp -o red_daft_lora$(python3-config --extension-suffix) \
 *       -DRD_USE_ROCM -lhip
 *
 * Build (CPU fallback, no GPU):
 *   c++ -O3 -Wall -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \
 *       src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \
 *       src/lora_pybind_wrapper.cpp -o red_daft_lora$(python3-config --extension-suffix)
 */

#include "red_daft_lora_manager.h"
#include "red_daft_vram.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace red_daft;

// ═══════════════════════════════════════════════════════════════════════
// Module-local singletons (initialized lazily)
// ═══════════════════════════════════════════════════════════════════════
static LoRaRegistry& get_registry() {
    static LoRaRegistry reg;
    return reg;
}

static LoRaSwapper& get_swapper() {
    static LoRaSwapper sw(get_registry(), VramEngine::instance());
    return sw;
}

static bool ensure_initialized() {
    auto& eng = VramEngine::instance();
    if (!eng.is_initialized()) {
        EngineConfig cfg;
        cfg.vram_budget_bytes = 0; // auto-detect
        cfg.host_budget_bytes = 0;
        cfg.num_streams = 4;
        return eng.initialize(cfg);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// LoRaAdapter info struct for Python (lightweight, copyable)
// ═══════════════════════════════════════════════════════════════════════
struct PyLoRaInfo {
    uint32_t    id;
    std::string name;
    int         pool;
    uint32_t    rank;
    uint32_t    in_features;
    uint32_t    out_features;
    size_t      total_size;
    std::string state;
    bool        on_vram;
    bool        on_host;
    uint32_t    access_count;
};

static PyLoRaInfo adapter_to_info(const LoRaAdapter& a) {
    PyLoRaInfo info;
    info.id = a.id;
    info.name = a.name;
    info.pool = static_cast<int>(a.pool);
    info.rank = a.rank;
    info.in_features = a.in_features;
    info.out_features = a.out_features;
    info.total_size = a.total_size;
    info.state = to_string(a.state);
    info.on_vram = a.is_on_vram();
    info.on_host = a.is_on_host();
    info.access_count = a.access_count;
    return info;
}

// ═══════════════════════════════════════════════════════════════════════
// LoRa Brain Engine summary stats for Python
// ═══════════════════════════════════════════════════════════════════════
struct PyLoRaStats {
    size_t total_adapters;
    size_t active_adapters;
    size_t vram_adapters;
    size_t total_vram_kb;
    size_t total_host_kb;
    uint64_t total_swaps;
    uint64_t total_evictions;
    uint64_t total_bytes_transferred;
    double  avg_swap_time_us;
    uint32_t active_id;
};

static PyLoRaStats collect_stats() {
    auto& reg = get_registry();
    auto& sw = get_swapper();
    auto s = sw.stats();
    PyLoRaStats ps;
    ps.total_adapters = reg.count();
    ps.active_adapters = reg.active_count();
    ps.vram_adapters = reg.vram_count();
    ps.total_vram_kb = reg.total_vram_bytes() >> 10;
    ps.total_host_kb = reg.total_host_bytes() >> 10;
    ps.total_swaps = s.total_swaps;
    ps.total_evictions = s.total_evictions;
    ps.total_bytes_transferred = s.total_bytes_transferred;
    ps.avg_swap_time_us = s.avg_swap_time_us;
    ps.active_id = sw.active_adapter_id();
    return ps;
}

// ═══════════════════════════════════════════════════════════════════════
// Pybind11 module: red_daft_lora
// ═══════════════════════════════════════════════════════════════════════
PYBIND11_MODULE(red_daft_lora, m) {
    m.doc() = R"pbdoc(
        Red Daft LoRa Brain Engine — microsecond hot-swap of LoRa adapters
        -----------------------------------------------------------------
        Manages LoRa adapter lifecycle across VRAM and DDR, with LRU
        eviction and async prefetching. Designed for 10–50 MB adapters
        running on a shared 1B–3B base model.

        Quick start:
            import red_daft_lora as lora
            lora.initialize()
            lora.register_lora("coding_v2", pool=3, rank=16, in_feat=256, out_feat=256)
            lora.register_lora("reasoning_v1", pool=2, rank=16, in_feat=256, out_feat=256)
            lora.swap_to_coding_lora()
            lora.swap_to_reasoning_lora()
            lora.evict_lru()
            lora.print_stats()

        Pools:
          1=SystemControl, 2=ReasoningLogic, 3=CodingSyntax, 4=ConversationLang

        Build (NVIDIA CUDA):
          c++ -O3 -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \\
              src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \\
              src/lora_pybind_wrapper.cpp -o red_daft_lora*.so \\
              -DRD_USE_CUDA -lcudart

        Build (AMD ROCm):
          c++ -O3 -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \\
              src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \\
              src/lora_pybind_wrapper.cpp -o red_daft_lora*.so \\
              -DRD_USE_ROCM -lhip

        Build (CPU fallback):
          c++ -O3 -shared -std=c++20 -fPIC $(python3 -m pybind11 --includes) \\
              src/red_daft_vram.cpp src/red_daft_lora_manager.cpp \\
              src/lora_pybind_wrapper.cpp -o red_daft_lora*.so
    )pbdoc";

    // ── LoRaPool enum ─────────────────────────────────────────────
    py::enum_<LoRaPool>(m, "LoRaPool")
        .value("SystemControl",    LoRaPool::SystemControl)
        .value("ReasoningLogic",   LoRaPool::ReasoningLogic)
        .value("CodingSyntax",     LoRaPool::CodingSyntax)
        .value("ConversationLang", LoRaPool::ConversationLang)
        .export_values();

    // ── LoRaState enum ────────────────────────────────────────────
    py::enum_<LoRaState>(m, "LoRaState")
        .value("Unregistered", LoRaState::Unregistered)
        .value("InDDR",        LoRaState::InDDR)
        .value("Loading",      LoRaState::Loading)
        .value("Active",       LoRaState::Active)
        .value("Evicting",     LoRaState::Evicting)
        .value("Evicted",      LoRaState::Evicted)
        .export_values();

    // ── PyLoRaInfo (copyable adapter info) ────────────────────────
    py::class_<PyLoRaInfo>(m, "LoRaInfo")
        .def_readonly("id",           &PyLoRaInfo::id)
        .def_readonly("name",         &PyLoRaInfo::name)
        .def_readonly("pool",         &PyLoRaInfo::pool)
        .def_readonly("rank",         &PyLoRaInfo::rank)
        .def_readonly("in_features",  &PyLoRaInfo::in_features)
        .def_readonly("out_features", &PyLoRaInfo::out_features)
        .def_readonly("total_size",   &PyLoRaInfo::total_size)
        .def_readonly("state",        &PyLoRaInfo::state)
        .def_readonly("on_vram",      &PyLoRaInfo::on_vram)
        .def_readonly("on_host",      &PyLoRaInfo::on_host)
        .def_readonly("access_count", &PyLoRaInfo::access_count)
        .def("__repr__", [](const PyLoRaInfo& i){
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "LoRaInfo(id=%u, name='%s', pool=%d, rank=%u, size=%zuKB, state=%s, vram=%s)",
                i.id, i.name.c_str(), i.pool, i.rank, i.total_size>>10,
                i.state.c_str(), i.on_vram ? "yes" : "no");
            return std::string(buf);
        });

    // ── PyLoRaStats ───────────────────────────────────────────────
    py::class_<PyLoRaStats>(m, "LoRaStats")
        .def_readonly("total_adapters",         &PyLoRaStats::total_adapters)
        .def_readonly("active_adapters",        &PyLoRaStats::active_adapters)
        .def_readonly("vram_adapters",          &PyLoRaStats::vram_adapters)
        .def_readonly("total_vram_kb",          &PyLoRaStats::total_vram_kb)
        .def_readonly("total_host_kb",          &PyLoRaStats::total_host_kb)
        .def_readonly("total_swaps",            &PyLoRaStats::total_swaps)
        .def_readonly("total_evictions",        &PyLoRaStats::total_evictions)
        .def_readonly("total_bytes_transferred", &PyLoRaStats::total_bytes_transferred)
        .def_readonly("avg_swap_time_us",       &PyLoRaStats::avg_swap_time_us)
        .def_readonly("active_id",              &PyLoRaStats::active_id)
        .def("__repr__", [](const PyLoRaStats& s){
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "LoRaStats(adapters=%zu active=%zu vram=%zu swaps=%llu evictions=%llu "
                "avg_swap=%.1fus active_id=%u)",
                s.total_adapters, s.active_adapters, s.vram_adapters,
                (unsigned long long)s.total_swaps, (unsigned long long)s.total_evictions,
                s.avg_swap_time_us, s.active_id);
            return std::string(buf);
        });

    // ── Lifecycle ─────────────────────────────────────────────────
    m.def("initialize", [](){
        return ensure_initialized();
    }, "Initialize VRAM engine + LoRa engine (auto-detect GPU). Returns True.");

    m.def("shutdown", [](){
        get_swapper().evict_all_except_active();
        get_swapper().synchronize();
        VramEngine::instance().shutdown();
    }, "Shutdown VRAM engine and LoRa engine.");

    m.def("is_initialized", [](){
        return VramEngine::instance().is_initialized();
    });

    // ── Registration ──────────────────────────────────────────────
    m.def("register_lora", [](const std::string& name, int pool,
                              uint32_t rank, uint32_t in_feat, uint32_t out_feat,
                              py::object a_data, py::object b_data) -> uint32_t {
        ensure_initialized();
        LoRaPool lp = static_cast<LoRaPool>(pool);

        // If numpy arrays are provided, extract data pointers
        if (!a_data.is_none()) {
            // Assume the caller passes a list or numpy array of floats
            // For simplicity, we accept a Python list and copy
            auto a_list = a_data.cast<std::vector<float>>();
            auto b_list = b_data.cast<std::vector<float>>();
            // We'll use the raw registration path
            return get_registry().register_adapter_raw(
                name, lp, rank, in_feat, out_feat,
                a_list.data(), a_list.size() * sizeof(float),
                b_list.data(), b_list.size() * sizeof(float));
        }
        return get_registry().register_empty(name, lp, rank, in_feat, out_feat);
    }, py::arg("name"), py::arg("pool"),
       py::arg("rank"), py::arg("in_feat"), py::arg("out_feat"),
       py::arg("a_data") = py::none(), py::arg("b_data") = py::none(),
       R"pbdoc(
        Register a LoRa adapter.
          name:      string identifier
          pool:      1=System, 2=Reasoning, 3=Coding, 4=Conversation
          rank:      LoRA rank r (typically 4–256)
          in_feat:   input dimension
          out_feat:  output dimension
          a_data:    optional list[float] for A matrix (rank × in_feat)
          b_data:    optional list[float] for B matrix (out_feat × rank)
        Returns adapter ID (uint32). 0 on failure.
       )pbdoc");

    m.def("register_lora_empty", [](const std::string& name, int pool,
                                    uint32_t rank, uint32_t in_feat, uint32_t out_feat) -> uint32_t {
        ensure_initialized();
        return get_registry().register_empty(
            name, static_cast<LoRaPool>(pool), rank, in_feat, out_feat);
    }, py::arg("name"), py::arg("pool"),
       py::arg("rank"), py::arg("in_feat"), py::arg("out_feat"),
       "Register an empty LoRa adapter (for lazy weight loading). Returns ID.");

    m.def("unregister", [](uint32_t id){
        get_registry().unregister(id);
    }, py::arg("id"), "Unregister and free an adapter.");

    // ── Lookup ────────────────────────────────────────────────────
    m.def("find_by_id", [](uint32_t id) -> py::object {
        auto* a = get_registry().find_by_id(id);
        if (!a) return py::none();
        return py::cast(adapter_to_info(*a));
    }, py::arg("id"), "Lookup adapter by ID. Returns LoRaInfo or None.");

    m.def("find_by_name", [](const std::string& name) -> py::object {
        auto* a = get_registry().find_by_name(name);
        if (!a) return py::none();
        return py::cast(adapter_to_info(*a));
    }, py::arg("name"), "Lookup adapter by name. Returns LoRaInfo or None.");

    m.def("count", [](){ return get_registry().count(); }, "Number of registered adapters.");

    m.def("list_adapters", []() -> std::vector<PyLoRaInfo> {
        std::vector<PyLoRaInfo> out;
        for (auto* a : get_registry().all_adapters()) {
            if (a) out.push_back(adapter_to_info(*a));
        }
        return out;
    }, "List all registered adapters as LoRaInfo objects.");

    // ── Hot-swap (core API) ───────────────────────────────────────
    m.def("swap_to", [](const std::string& name) -> bool {
        ensure_initialized();
        auto* a = get_registry().find_by_name(name);
        if (!a) {
            std::fprintf(stderr, "[lora-py] adapter '%s' not found\n", name.c_str());
            return false;
        }
        return get_swapper().hot_swap_async(a->id);
    }, py::arg("name"), "Hot-swap to adapter by name (async, returns immediately).");

    m.def("swap_to_id", [](uint32_t id) -> bool {
        ensure_initialized();
        return get_swapper().hot_swap_async(id);
    }, py::arg("id"), "Hot-swap to adapter by ID (async).");

    // ── Named pool swaps (convenience) ────────────────────────────
    m.def("swap_to_system_lora", [](){
        ensure_initialized();
        return get_swapper().swap_to_system_lora();
    }, "Hot-swap to System/Control LoRa (pool 1).");

    m.def("swap_to_reasoning_lora", [](){
        ensure_initialized();
        return get_swapper().swap_to_reasoning_lora();
    }, "Hot-swap to Reasoning/Logic LoRa (pool 2).");

    m.def("swap_to_coding_lora", [](){
        ensure_initialized();
        return get_swapper().swap_to_coding_lora();
    }, "Hot-swap to Coding/Syntax LoRa (pool 3).");

    m.def("swap_to_conversation_lora", [](){
        ensure_initialized();
        return get_swapper().swap_to_conversation_lora();
    }, "Hot-swap to Conversation/Language LoRa (pool 4).");

    // ── SGMV weight patching ────────────────────────────────────────
    m.def("apply_lora_weights", [](uint32_t id, py::object output, py::object input,
                                   size_t batch_size) -> bool {
        ensure_initialized();
        // Take the caller's list, patch it in place, and return the result.
        py::list out_list = output.cast<py::list>();
        const size_t n = out_list.size();
        std::vector<float> out(n);
        for (size_t i = 0; i < n; ++i) out[i] = out_list[i].cast<float>();
        auto inp = input.cast<std::vector<float>>();
        bool ok = get_swapper().apply_lora_weights(id, out.data(), inp.data(), batch_size);
        if (ok) {
            for (size_t i = 0; i < n; ++i) out_list[i] = py::cast(out[i]);
        }
        return ok;
    }, py::arg("id"), py::arg("output"), py::arg("input"), py::arg("batch_size") = 1,
       R"pbdoc(
        Apply LoRA weights (SGMV) on the fly: output = base_output + B @ A @ input.
          id:         adapter ID
          output:     list[float] of length out_feat — receives base_output + patch
          input:      list[float] of length in_feat — the input activation
          batch_size: number of rows (default 1)
        Does NOT modify the base model weights in Pool 0.
        Returns True on success.
       )pbdoc");

    // ── Eviction ──────────────────────────────────────────────────
    m.def("evict_lru", [](){
        return get_swapper().evict_lru_lora();
    }, "Evict the Least Recently Used adapter from VRAM to DDR.");

    m.def("evict_adapter", [](uint32_t id){
        return get_swapper().evict_adapter(id);
    }, py::arg("id"), "Evict a specific adapter from VRAM to DDR.");

    m.def("evict_all", []() -> size_t {
        return get_swapper().evict_all_except_active();
    }, "Evict all non-active adapters. Returns count evicted.");

    // ── Synchronization ───────────────────────────────────────────
    m.def("synchronize", [](){
        get_swapper().synchronize();
    }, "Wait for all pending async transfers to complete.");

    m.def("wait_ready", [](uint32_t id, int timeout_ms) -> bool {
        return get_swapper().wait_adapter_ready(id, timeout_ms);
    }, py::arg("id"), py::arg("timeout_ms") = 5000,
       "Block until adapter is in VRAM and ready.");

    // ── Introspection ─────────────────────────────────────────────
    m.def("active_adapter", []() -> py::object {
        auto* a = get_swapper().active_adapter();
        if (!a) return py::none();
        return py::cast(adapter_to_info(*a));
    }, "Returns LoRaInfo for the currently active adapter, or None.");

    m.def("is_in_vram", [](uint32_t id) -> bool {
        return get_swapper().is_in_vram(id);
    }, py::arg("id"), "Check if adapter is currently resident in VRAM.");

    // ── Stats ─────────────────────────────────────────────────────
    m.def("lora_stats", []() -> PyLoRaStats {
        return collect_stats();
    }, "Return LoRa engine statistics as LoRaStats.");

    m.def("print_stats", [](){
        get_swapper().print_stats();
    }, "Print LoRa engine stats table to stdout.");

    m.def("reset_stats", [](){
        get_swapper().reset_stats();
    }, "Reset swap/eviction counters.");

    // ── Python iteration support ──────────────────────────────────
    m.def("adapter_names", []() -> std::vector<std::string> {
        std::vector<std::string> out;
        for (auto* a : get_registry().all_adapters())
            if (a) out.push_back(a->name);
        return out;
    }, "List all adapter names.");

    m.def("adapter_ids", []() -> std::vector<uint32_t> {
        std::vector<uint32_t> out;
        for (auto* a : get_registry().all_adapters())
            if (a) out.push_back(a->id);
        return out;
    }, "List all adapter IDs.");
}
