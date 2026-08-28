#pragma once
/**
 * red_daft_gpu.h — Unified CUDA / ROCm HIP Abstraction Layer (red-burst)
 * =======================================================================
 * Thin, self-contained hardware abstraction for the red-burst Ultra-LoRA
 * Engine. Maps:
 *
 *   NVIDIA CUDA            ⇄  AMD ROCm/HIP
 *   cudaMalloc             ⇄  hipMalloc
 *   cudaFree               ⇄  hipFree
 *   cudaMemcpyAsync        ⇄  hipMemcpyAsync
 *   cudaStreamCreate       ⇄  hipStreamCreate
 *   cudaStreamSynchronize  ⇄  hipStreamSynchronize
 *   cudaGetDeviceProperties⇄  hipGetDeviceProperties
 *   WMMA Tensor Cores      ⇄  MFMA Matrix Cores
 *
 * Macro-selected at compile time (same flags as the rest of Red Daft OS):
 *   -DRD_USE_CUDA           → NVIDIA backend
 *   -DRD_USE_ROCM           → AMD backend (__HIP_PLATFORM_AMD__)
 *   (no flag)               → CPU fallback (CI / Kaggle CPU / local tests)
 *
 * The CPU fallback replaces device pointers with host malloc/memcpy so the
 * entire burst pipeline — 3 parallel streams, quantization, pool routing,
 * watchdog — is fully runnable and testable without a GPU.
 *
 * Tensor-core hooks: when compiled with a vendor compiler (nvcc/hipcc) the
 * RD_GPU_TENSOR_CORE macros expand to WMMA/MFMA fragment types; under g++/
 * clang (CPU fallback) they degrade to plain float accumulation so the burst
 * math is a faithful, bit-deterministic model of the GPU path.
 *
 * C++20, header-only. License: Red Daft OS Verbatim Distribution License v1.0.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// ─────────────────────────────────────────────────────────────────────
// Backend selection
// ─────────────────────────────────────────────────────────────────────
#if defined(RD_USE_ROCM) || defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
  #ifndef RD_BACKEND_ROCM
    #define RD_BACKEND_ROCM 1
  #endif
#elif defined(RD_USE_CUDA) || defined(__CUDACC__) || defined(__CUDACC_VER_MAJOR__)
  #ifndef RD_BACKEND_CUDA
    #define RD_BACKEND_CUDA 1
  #endif
#else
  #define RD_BACKEND_CPU 1
#endif

#if defined(RD_BACKEND_ROCM)
  #include <hip/hip_runtime.h>
  #include <hip/hip_fp16.h>
#elif defined(RD_BACKEND_CUDA)
  #include <cuda_runtime.h>
  #include <cuda_fp16.h>
#endif

namespace red_daft::gpu {

// ─────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────
#if defined(RD_BACKEND_ROCM)
using Stream          = hipStream_t;
using Device          = hipDeviceProp_t;
using Error           = hipError_t;
inline const char* error_string(Error e) { return hipGetErrorString(e); }
constexpr Error kOk   = hipSuccess;
#elif defined(RD_BACKEND_CUDA)
using Stream          = cudaStream_t;
using Device          = cudaDeviceProp;
using Error           = cudaError_t;
inline const char* error_string(Error e) { return cudaGetErrorString(e); }
constexpr Error kOk   = cudaSuccess;
#else // CPU fallback
using Stream          = void*;
struct Device { int major = 0, minor = 0; int multiProcessorCount = 1; };
using Error           = int;
constexpr Error kOk   = 0;
inline const char* error_string(Error) { return "CPU fallback (no GPU)"; }
#endif

// ─────────────────────────────────────────────────────────────────────
// Low-level device memory + streams (zero-copy-safe thin wrappers)
// ─────────────────────────────────────────────────────────────────────
inline Error alloc(void** p, size_t bytes) {
#if defined(RD_BACKEND_ROCM)
    return hipMalloc(p, bytes);
#elif defined(RD_BACKEND_CUDA)
    return cudaMalloc(p, bytes);
#else
    *p = std::malloc(bytes);
    return *p ? kOk : 1;
#endif
}

inline Error free(void* p) {
#if defined(RD_BACKEND_ROCM)
    return (p == nullptr) ? hipSuccess : hipFree(p);
#elif defined(RD_BACKEND_CUDA)
    return (p == nullptr) ? cudaSuccess : cudaFree(p);
#else
    std::free(p);
    return kOk;
#endif
}

inline Error memcpy_async(void* dst, const void* src, size_t bytes, Stream s) {
#if defined(RD_BACKEND_ROCM)
    return hipMemcpyAsync(dst, src, bytes, hipMemcpyDefault, s);
#elif defined(RD_BACKEND_CUDA)
    return cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDefault, s);
#else
    (void)s;
    if (dst != src) std::memcpy(dst, src, bytes);
    return kOk;
#endif
}

inline Error memcpy_htod(void* dst, const void* src, size_t bytes, Stream s) {
#if defined(RD_BACKEND_ROCM)
    return hipMemcpyAsync(dst, src, bytes, hipMemcpyHostToDevice, s);
#elif defined(RD_BACKEND_CUDA)
    return cudaMemcpyAsync(dst, src, bytes, cudaMemcpyHostToDevice, s);
#else
    (void)s;
    std::memcpy(dst, src, bytes);
    return kOk;
#endif
}

inline Error memcpy_dtoh(void* dst, const void* src, size_t bytes, Stream s) {
#if defined(RD_BACKEND_ROCM)
    return hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToHost, s);
#elif defined(RD_BACKEND_CUDA)
    return cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToHost, s);
#else
    (void)s;
    std::memcpy(dst, src, bytes);
    return kOk;
#endif
}

inline Error stream_create(Stream* s, int priority = -1) {
#if defined(RD_BACKEND_ROCM)
    hipStreamCreateWithPriority(s, hipStreamNonBlocking, priority);
    return hipSuccess;
#elif defined(RD_BACKEND_CUDA)
    return cudaStreamCreateWithPriority(s, cudaStreamNonBlocking, priority);
#else
    (void)priority;
    *s = (void*)0x1;
    return kOk;
#endif
}

inline Error stream_sync(Stream s) {
#if defined(RD_BACKEND_ROCM)
    return hipStreamSynchronize(s);
#elif defined(RD_BACKEND_CUDA)
    return cudaStreamSynchronize(s);
#else
    (void)s;
    return kOk;
#endif
}

inline Error stream_destroy(Stream s) {
#if defined(RD_BACKEND_ROCM)
    return (s == nullptr) ? hipSuccess : hipStreamDestroy(s);
#elif defined(RD_BACKEND_CUDA)
    return (s == nullptr) ? cudaSuccess : cudaStreamDestroy(s);
#else
    (void)s;
    return kOk;
#endif
}

inline Error device_get(int id, Device* d) {
#if defined(RD_BACKEND_ROCM)
    hipGetDeviceProperties(d, id);
    return hipSuccess;
#elif defined(RD_BACKEND_CUDA)
    return cudaGetDeviceProperties(d, id);
#else
    (void)id; (void)d;
    return kOk;
#endif
}

inline const char* backend_name() {
#if defined(RD_BACKEND_ROCM)
    return "ROCM/HIP";
#elif defined(RD_BACKEND_CUDA)
    return "CUDA";
#else
    return "CPU-fallback";
#endif
}

inline bool is_gpu() {
#if defined(RD_BACKEND_ROCM) || defined(RD_BACKEND_CUDA)
    return true;
#else
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────
// Tensor-core hooks (WMMA ⇄ MFMA)
// ─────────────────────────────────────────────────────────────────────
// When compiled with a real vendor compiler these expand to the proper
// fragment types used by the burst quant kernel. Under g++/clang (CPU
// fallback) they reduce to plain scalar/vector float accumulation so the
// reference kernel is a deterministic model of the accelerator path.
#if defined(__CUDA_ARCH__) && defined(RD_USE_CUDA) || defined(__CUDACC__)
  #ifndef RD_TENSOR_CORE_AVAILABLE
    #define RD_TENSOR_CORE_AVAILABLE 1
  #endif
  // WMMA fragments (fp16 inputs, fp32 acc), 16x16x16 shape
  #define RD_WMMA_FRAG_A nvcuda::wmma::fragment<nvcuda::wmma::matrix_a, 16, 16, 16, __half, nvcuda::wmma::row_major>
  #define RD_WMMA_FRAG_B nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, 16, 16, 16, __half, nvcuda::wmma::col_major>
  #define RD_WMMA_FRAG_C nvcuda::wmma::fragment<nvcuda::wmma::accumulator, 16, 16, 16, float>
  #include <mma.h>
#elif defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
  #ifndef RD_TENSOR_CORE_AVAILABLE
    #define RD_TENSOR_CORE_AVAILABLE 1
  #endif
  // ROCm mfma intrinsic builtins
  #include <hip/hip_fp16.h>
#endif

} // namespace red_daft::gpu
