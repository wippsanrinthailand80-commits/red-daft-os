cmake_minimum_required(VERSION 3.18)
project(red_daft_vram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Backend selection ──────────────────────────────────────────────────
option(USE_CUDA "Enable NVIDIA CUDA backend" ON)
option(USE_ROCM "Enable AMD ROCm HIP backend" OFF)
option(BUILD_PYBIND "Build Python extension via pybind11" ON)

if(USE_ROCM)
  add_definitions(-DRD_USE_ROCM)
  find_package(hip REQUIRED)
elseif(USE_CUDA)
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    add_definitions(-DRD_USE_CUDA)
  else()
    message(STATUS "[VRAM] CUDA not found — building CPU fallback (still benchmarks correctly)")
  endif()
endif()

# ── Core library (+ LoRa Brain Engine) ───────────────────────────────
add_library(red_daft_vram STATIC
  src/red_daft_vram.cpp
  src/red_daft_lora_manager.cpp
)
target_include_directories(red_daft_vram PUBLIC include)
target_compile_options(red_daft_vram PRIVATE -O3 -Wall -Wextra)

if(USE_ROCM)
  target_link_libraries(red_daft_vram PUBLIC hip::host hip::device)
elseif(CUDAToolkit_FOUND)
  target_link_libraries(red_daft_vram PUBLIC CUDA::cudart)
endif()

# ── Pybind11 extension ─────────────────────────────────────────────────
if(BUILD_PYBIND)
  find_package(pybind11 QUIET)
  if(NOT pybind11_FOUND)
    # Fetch pybind11 if not installed
    include(FetchContent)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG v2.12.0
    )
    FetchContent_MakeAvailable(pybind11)
  endif()

  # Try to find Torch for optional torch integration
  find_package(Torch QUIET)
  if(Torch_FOUND)
    message(STATUS "[VRAM] Torch found — enabling torch integration in pybind")
  endif()

  pybind11_add_module(red_daft_vram_py src/pybind_wrapper.cpp src/lora_pybind_wrapper.cpp)
  target_link_libraries(red_daft_vram_py PRIVATE red_daft_vram)
  target_include_directories(red_daft_vram_py PRIVATE include)

  if(USE_ROCM)
    target_link_libraries(red_daft_vram_py PRIVATE hip::host hip::device)
  elseif(CUDAToolkit_FOUND)
    target_link_libraries(red_daft_vram_py PRIVATE CUDA::cudart)
  endif()

  if(Torch_FOUND)
    target_link_libraries(red_daft_vram_py PRIVATE ${TORCH_LIBRARIES})
    target_compile_definitions(red_daft_vram_py PRIVATE RD_HAS_TORCH=1)
  endif()

  # Output name: red_daft_vram.*.so (importable as `import red_daft_vram`)
  set_target_properties(red_daft_vram_py PROPERTIES OUTPUT_NAME "red_daft_vram")
endif()

# ── Native test binaries (CPU fallback, no Python) ───────────────────
add_executable(vram_test tests/test_vram.cpp)
target_link_libraries(vram_test PRIVATE red_daft_vram)

add_executable(lora_test tests/test_lora.cpp)
target_link_libraries(lora_test PRIVATE red_daft_vram)

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS red_daft_vram DESTINATION lib)
install(FILES include/red_daft_vram.h include/red_daft_lora_manager.h DESTINATION include)
if(BUILD_PYBIND)
  install(TARGETS red_daft_vram_py DESTINATION .)
endif()
