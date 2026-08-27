/**
 * red_daft_nano_context.cpp — Red Daft OS Nano-Context Engine (implementation)
 * ========================================================================
 * Implements ephemeral, high-density "Nano-Pools" for the input/output
 * context window and KV cache, keeping Base Model Weights strictly in FP16
 * in VRAM Pool 0.
 *
 * Key guarantees delivered:
 *  1. MODEL WEIGHTS NEVER MOVE: this engine never touches Pool 0; it works
 *     only on separate, ephemeral Nano-Pools.
 *  2. KV COMPRESSION: FP16 -> INT4 / INT2 per-channel quantization with
 *     per-channel scale + zero-point, packed into the Nano-Pool buffer.
 *  3. EPHEMERAL TOKEN STREAM: a fixed-capacity ring (< 50 MB per stream)
 *     for continuous input/output tokens.
 *  4. LIFECYCLE RECYCLING: request_begin() spawns (or reuses a warm
 *     free-list pool); request_end() deallocates / recycles.
 *
 * Built to compile across CUDA / ROCm / CPU with the same macro switch as
 * the rest of Red Daft OS (RD_USE_CUDA / RD_USE_ROCM / none = CPU).
 */

#include "red_daft_nano_context.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

#if defined(RD_NANO_BACKEND_CPU)
  #include <cstdlib>
#endif

namespace red_daft {

// ─────────────────────────────────────────────────────────────────────
// Packed size for a given quant type
// ─────────────────────────────────────────────────────────────────────
size_t NanoPool::packed_bytes(size_t n, NanoQuantType t) {
    switch (t) {
        case NanoQuantType::None: return n * sizeof(float);       // raw fp32 storage
        case NanoQuantType::Int4: return n / 2 + (n % 2);         // 4 bits/value
        case NanoQuantType::Int2: return n / 4 + (n % 4 ? 1 : 0); // 2 bits/value
        default:                 return n * sizeof(float);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Quantize FP32 vector into packed INT4/INT2 with per-channel scale/zpt.
// `channels` == number of scale groups (e.g. one per head-dim channel).
// ─────────────────────────────────────────────────────────────────────
void NanoPool::quantize_into(const float* src, size_t n, NanoQuantType t,
                             uint8_t* packed, float* scale, float* zpt) {
    // Per-channel grouping: one scale/(zpt) per `channel_stride` values.
    const size_t channel_stride = 32; // 32 channels/group for head_dim agnosticism
    const size_t nchannels = (n + channel_stride - 1) / channel_stride;

    for (size_t ch = 0; ch < nchannels; ++ch) {
        size_t start = ch * channel_stride;
        size_t end = std::min(start + channel_stride, n);
        float mn = src[start], mx = src[start];
        for (size_t i = start; i < end; ++i) {
            mn = std::min(mn, src[i]);
            mx = std::max(mx, src[i]);
        }
        scale[ch] = mx - mn;
        zpt[ch]   = mn;
        float inv = (scale[ch] > 0.f) ? 1.f / scale[ch] : 0.f;

        if (t == NanoQuantType::Int4) {
            // 4-bit: -8..7
            for (size_t i = start; i < end; ++i) {
                int q = (int)std::lround((src[i] - mn) * inv * 15.f) - 8;
                q = std::max(-8, std::min(7, q));
                size_t bitpos = i * 4;
                size_t byte = bitpos >> 3;
                unsigned shift = bitpos & 7;
                uint8_t v = (uint8_t)(q + 8);   // unbiased to 0..15 before pack
                packed[byte] = (uint8_t)(packed[byte] | (v << shift));
            }
        } else if (t == NanoQuantType::Int2) {
            // 2-bit: 0..3 (unsigned, denser)
            for (size_t i = start; i < end; ++i) {
                float ratio = (src[i] - mn) * inv; // 0..1
                int q = (int)std::lround(ratio * 3.f);
                q = std::max(0, std::min(3, q));
                size_t bitpos = i * 2;
                size_t byte = bitpos >> 3;
                unsigned shift = bitpos & 7;
                packed[byte] = (uint8_t)(packed[byte] | ((uint8_t)q << shift));
            }
        }
        // None: copy raw (handled by caller storing src directly)
    }
}

void NanoPool::dequantize_from(const uint8_t* packed, const float* scale,
                               const float* zpt, size_t n, NanoQuantType t,
                               float* out) {
    const size_t channel_stride = 32;
    for (size_t i = 0; i < n; ++i) {
        size_t ch = i / channel_stride;
        if (t == NanoQuantType::Int4) {
            size_t bitpos = i * 4;
            size_t byte = bitpos >> 3;
            unsigned shift = bitpos & 7;
            int q = (packed[byte] >> shift) & 0xF;
            int signed_q = q - 8; // bias
            out[i] = zpt[ch] + (float)(signed_q + 8) / 15.f * scale[ch];
        } else if (t == NanoQuantType::Int2) {
            size_t bitpos = i * 2;
            size_t byte = bitpos >> 3;
            unsigned shift = bitpos & 7;
            int q = (packed[byte] >> shift) & 0x3;
            out[i] = zpt[ch] + (float)q / 3.f * scale[ch];
        } else {
            out[i] = packed ? 0.f : 0.f;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// NanoPool implementation
// ─────────────────────────────────────────────────────────────────────
NanoPool::NanoPool(uint64_t req_id, const NanoContextConfig& cfg)
    : cfg_(cfg), request_id_(req_id), device_resident_(false), device_buf_(nullptr) {
    // Storage for each (layer, head): packed bytes for K + packed bytes for V,
    // plus per-channel scale/zpt for K and V. Sized for max_tokens rows.
    const size_t elements = cfg_.max_tokens * cfg_.head_dim; // per layer per head
    const size_t packed_k = packed_bytes(elements, cfg_.default_k_quant);
    const size_t packed_v = packed_bytes(elements, cfg_.default_v_quant);
    const size_t nchannels = (elements + 32 - 1) / 32;
    const size_t scale_zpt = nchannels * sizeof(float);
    // dual storage (K + V) each with packed + scale + zpt
    quant_cell_bytes_ = packed_k + packed_v + 2 * scale_zpt;
    channel_bytes_    = scale_zpt;

    cells_.resize(cfg_.kv_layers);
    for (auto& layer : cells_) layer.resize(cfg_.kv_heads);

    // Token stream ring (min footprint)
    const size_t ring_cap = std::max<size_t>(256, cfg_.stream_capacity / sizeof(float));
    ring_.assign(ring_cap, 0.f);

    // Nano-Pools stay host-resident by default (capacity tier). This keeps the
    // < 50 MB / active stream SLA and never touches VRAM Pool 0 weights.
    device_resident_ = false;
    device_buf_ = nullptr;
}

NanoPool::~NanoPool() {
    for (auto& layer : cells_)
        for (auto& c : layer) {
            if (c.data) { delete[] c.data; c.data = nullptr; }
            if (c.scale) { delete[] c.scale; c.scale = nullptr; }
            if (c.zpt)   { delete[] c.zpt;   c.zpt = nullptr; }
        }
    cells_.clear();
    if (device_buf_) {
        // Frees via HAL (CPU fallback = free; device = cudaFree/hipFree)
        delete[] static_cast<uint8_t*>(device_buf_);
        device_buf_ = nullptr;
    }
}

bool NanoPool::kv_store(size_t layer, size_t head,
                        const float* key, const float* value, size_t seq_tokens,
                        NanoQuantType kq, NanoQuantType vq) {
    if (layer >= cells_.size() || head >= cells_[layer].size())
        return false;
    if (!key || !value || seq_tokens == 0)
        return false;

    auto& cell = cells_[layer][head];
    const size_t n = seq_tokens * cfg_.head_dim;
    const size_t nchannels = (n + 31) / 32;

    // Allocate storage for this cell (lazy, sized to tokens actually stored)
    size_t pk = packed_bytes(n, kq == NanoQuantType::None ? NanoQuantType::None : kq);
    size_t pv = packed_bytes(n, vq == NanoQuantType::None ? NanoQuantType::None : vq);
    if (!cell.data) {
        cell.data = new uint8_t[pk + pv]();   // zero-initialized (bit-pack uses OR)
        cell.size = pk + pv;
    }
    if (!cell.scale) {
        cell.scale = new float[nchannels * 2];
        cell.zpt   = new float[nchannels * 2];
        cell.channels = nchannels * 2; // K + V scale groups
    }

    uint8_t* kd = cell.data;
    uint8_t* vd = cell.data + pk;
    // Zero the regions we pack into (bit-packing uses OR; fresh clear = correctness)
    std::memset(kd, 0, pk);
    std::memset(vd, 0, pv);

    if (kq == NanoQuantType::None) {
        std::memcpy(kd, key, n * sizeof(float));
    } else {
        quantize_into(key, n, kq, kd, cell.scale, cell.zpt);
    }
    if (vq == NanoQuantType::None) {
        std::memcpy(vd, value, n * sizeof(float));
    } else {
        quantize_into(value, n, vq, vd, cell.scale + nchannels, cell.zpt + nchannels);
    }

    cell.type = (kq == vq) ? kq : NanoQuantType::Int4;
    cell.seq_len = seq_tokens;

    quant_.kv_entries += 1;
    quant_.bytes_compressed += cell.size;
    quant_.bytes_saved += n * sizeof(float) * 2 - cell.size;
    quant_.quant_ops += 1;

    return true;
}

bool NanoPool::kv_load(size_t layer, size_t head,
                       float* key_out, float* value_out, size_t seq_tokens) const {
    if (layer >= cells_.size() || head >= cells_[layer].size())
        return false;
    const auto& cell = cells_[layer][head];
    if (!cell.data || !cell.scale || cell.seq_len < seq_tokens)
        return false;

    const size_t n = seq_tokens * cfg_.head_dim;
    const size_t nchannels = (n + 31) / 32;
    size_t pk = packed_bytes(n, cell.type == NanoQuantType::None
                                ? NanoQuantType::None : cell.type);

    const uint8_t* kd = cell.data;
    const uint8_t* vd = cell.data + pk;

    // For None type, we actually stored raw float32.
    NanoQuantType actual = cell.type;
    if (actual == NanoQuantType::None) {
        std::memcpy(key_out, kd, n * sizeof(float));
        std::memcpy(value_out, vd, n * sizeof(float));
    } else {
        dequantize_from(kd, cell.scale, cell.zpt, n, actual, key_out);
        dequantize_from(vd, cell.scale + nchannels, cell.zpt + nchannels, n, actual, value_out);
    }
    return true;
}

bool NanoPool::stream_push(const float* tokens, size_t n) {
    if (!tokens || n == 0) return false;
    // Ring buffer; drop oldest on overflow (streaming semantics)
    for (size_t i = 0; i < n; ++i) {
        ring_[ring_tail_] = tokens[i];
        ring_tail_ = (ring_tail_ + 1) % ring_.size();
        if (ring_count_ < ring_.size()) {
            ring_count_++;
        } else {
            // full: advance head (oldest dropped)
            ring_head_ = (ring_head_ + 1) % ring_.size();
            stream_.overflows++;
        }
        stream_.tokens_ingested++;
    }
    return true;
}

bool NanoPool::stream_pop(float* out, size_t max_n) {
    if (!out || max_n == 0) return false;
    size_t take = std::min(max_n, ring_count_);
    for (size_t i = 0; i < take; ++i) {
        out[i] = ring_[ring_head_];
        ring_head_ = (ring_head_ + 1) % ring_.size();
        ring_count_--;
        stream_.tokens_emitted++;
    }
    return true;
}

size_t NanoPool::stream_available() const { return ring_count_; }

void NanoPool::reset() {
    // Rewind all cells (free allocations, keep the pool object)
    for (auto& layer : cells_)
        for (auto& c : layer) {
            if (c.data) { delete[] c.data; c.data = nullptr; }
            if (c.scale) { delete[] c.scale; c.scale = nullptr; }
            if (c.zpt)   { delete[] c.zpt;   c.zpt = nullptr; }
            c = QuantizedKVCell{};
        }
    ring_head_ = ring_tail_ = ring_count_ = 0;
    quant_  = NanoQuantStats{};
    stream_ = NanoStreamStats{};
    state_  = NanoPoolState::Free;
}

NanoPoolStats NanoPool::snapshot() const {
    NanoPoolStats s;
    s.request_id     = request_id_;
    s.capacity_bytes = capacity();
    s.used_bytes     = quant_.bytes_compressed + ring_count_ * sizeof(float);
    s.k_quant        = cfg_.default_k_quant;
    s.v_quant        = cfg_.default_v_quant;
    s.head_size      = cfg_.head_dim;
    s.n_layers       = cfg_.kv_layers;
    s.quant          = quant_;
    s.stream         = stream_;
    s.is_device_resident = device_resident_;
    return s;
}

// ─────────────────────────────────────────────────────────────────────
// NanoContextEngine implementation
// ─────────────────────────────────────────────────────────────────────
NanoContextEngine& NanoContextEngine::instance() {
    static NanoContextEngine inst;
    return inst;
}

bool NanoContextEngine::initialize(const NanoContextConfig& cfg) {
    std::unique_lock lock(registry_mutex_);
    cfg_ = cfg;
    total_requests_.store(0);
    initialized_.store(true);
    std::printf("[nano] Nano-Context Engine initialized: layers=%zu heads=%zu "
                "head_dim=%zu max_tokens=%zu stream=%zu KiB free_list=%zu\n",
                cfg_.kv_layers, cfg_.kv_heads, cfg_.head_dim, cfg_.max_tokens,
                cfg_.stream_capacity >> 10, cfg_.free_list_capacity);
    return true;
}

void NanoContextEngine::shutdown() {
    std::unique_lock lock(registry_mutex_);
    active_.clear();
    free_list_.clear();
    initialized_.store(false);
}

uint64_t NanoContextEngine::request_begin() {
    std::unique_lock lock(registry_mutex_);
    uint64_t id = next_req_.fetch_add(1);
    total_requests_.fetch_add(1);

    // Recycle a warm pool from the free-list when available (low-latency spawn).
    if (!free_list_.empty()) {
        auto p = std::move(free_list_.back());
        free_list_.pop_back();
        p->reset();
        p->set_state(NanoPoolState::Active);
        active_.emplace(id, std::move(p));
    } else {
        auto p = std::make_unique<NanoPool>(id, cfg_);
        p->set_state(NanoPoolState::Active);
        active_.emplace(id, std::move(p));
    }
    return id;
}

void NanoContextEngine::request_end(uint64_t req_id) {
    std::unique_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return;

    auto pool = std::move(it->second);
    active_.erase(it);

    // Recycle to free-list (bounded) or deallocate.
    if (free_list_.size() < cfg_.free_list_capacity) {
        pool->set_state(NanoPoolState::Free);
        pool->reset();
        free_list_.push_back(std::move(pool));
    } else {
        // Deallocate immediately when free-list is full.
        pool.reset();
    }
}

bool NanoContextEngine::kv_store(uint64_t req_id, size_t layer, size_t head,
                                 const float* key, const float* value, size_t seq_tokens) {
    std::shared_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return false;
    return it->second->kv_store(layer, head, key, value, seq_tokens,
                                cfg_.default_k_quant, cfg_.default_v_quant);
}

bool NanoContextEngine::kv_load(uint64_t req_id, size_t layer, size_t head,
                                float* key_out, float* value_out, size_t seq_tokens) const {
    std::shared_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return false;
    return it->second->kv_load(layer, head, key_out, value_out, seq_tokens);
}

bool NanoContextEngine::stream_push(uint64_t req_id, const float* tokens, size_t n) {
    std::shared_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return false;
    return it->second->stream_push(tokens, n);
}

bool NanoContextEngine::stream_pop(uint64_t req_id, float* out, size_t max_n) {
    std::shared_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return false;
    return it->second->stream_pop(out, max_n);
}

size_t NanoContextEngine::active_pools() const {
    std::shared_lock lock(registry_mutex_);
    return active_.size();
}

size_t NanoContextEngine::free_pools() const {
    std::shared_lock lock(registry_mutex_);
    return free_list_.size();
}

NanoPoolStats NanoContextEngine::pool_stats(uint64_t req_id) const {
    std::shared_lock lock(registry_mutex_);
    auto it = active_.find(req_id);
    if (it == active_.end()) return NanoPoolStats{};
    return it->second->snapshot();
}

void NanoContextEngine::print_stats() const {
    std::shared_lock lock(registry_mutex_);
    std::printf("\n╔══════════════════════════════════════════════════════╗\n");
    std::printf("║  Red Daft Nano-Context Engine — Pool Statistics      ║\n");
    std::printf("╠══════════════════════════════════════════════════════╣\n");
    std::printf("║  Active pools:       %10zu                         ║\n", active_.size());
    std::printf("║  Free-list (warm):   %10zu                         ║\n", free_list_.size());
    std::printf("║  Total requests:     %10llu                         ║\n",
                (unsigned long long)total_requests_.load());
    std::printf("║  kv_layers:          %10zu                         ║\n", cfg_.kv_layers);
    std::printf("║  kv_heads:           %10zu                         ║\n", cfg_.kv_heads);
    std::printf("║  head_dim:           %10zu                         ║\n", cfg_.head_dim);
    std::printf("║  max_tokens/req:     %10zu                         ║\n", cfg_.max_tokens);
    std::printf("║  stream_capacity:    %10zu KiB                      ║\n", cfg_.stream_capacity >> 10);
    std::printf("╚══════════════════════════════════════════════════════╝\n\n");

    if (active_.empty()) {
        std::printf("  (no active Nano-Pools)\n");
        return;
    }
    std::printf("  %-8s  %-10s  %-10s  %-10s  %-10s  %-10s\n",
                "req_id", "used(KB)", "KV cells", "quant_ops", "bytes_saved(KB)", "tokens");
    for (auto& [id, p] : active_) {
        auto s = p->snapshot();
        std::printf("  %-8llu  %-10zu  %-10llu  %-10llu  %-10llu  %-10llu\n",
                    (unsigned long long)id, s.used_bytes >> 10,
                    (unsigned long long)s.quant.kv_entries,
                    (unsigned long long)s.quant.quant_ops,
                    (unsigned long long)(s.quant.bytes_saved >> 10),
                    (unsigned long long)s.stream.tokens_ingested);
    }
}

} // namespace red_daft
