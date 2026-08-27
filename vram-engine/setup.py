#!/usr/bin/env python3
"""
setup.py — Build Red Daft VRAM Engine Python extension
Usage:
  pip install pybind11 torch  # optional but recommended
  pip install -e .            # builds red_daft_vram*.so in-place
  # or with CUDA:
  RD_USE_CUDA=1 pip install -e .
  # or ROCm:
  RD_USE_ROCM=1 pip install -e .

Kaggle one-liner:
  !git clone https://github.com/wippsanrinthailand80-commits/red-daft-os.git
  !cd red-daft-os/vram-engine && pip install pybind11 && pip install -e .
"""
from setuptools import setup, Extension
import sys, os, pathlib

try:
    import pybind11
    include_pybind = pybind11.get_include()
except ImportError:
    include_pybind = "/usr/local/include"

# Detect backend
extra_compile_args = ["-O3", "-std=c++20", "-fPIC", "-Wall", "-Wextra"]
define_macros = []
libraries = []

if os.environ.get("RD_USE_ROCM") == "1":
    define_macros.append(("RD_USE_ROCM", "1"))
    libraries += ["amdhip64"]
elif os.environ.get("RD_USE_CUDA") == "1":
    define_macros.append(("RD_USE_CUDA", "1"))
    libraries += ["cudart"]
else:
    # Auto-detect: check for ROCm first (AMD), then CUDA (NVIDIA), then CPU fallback
    import glob
    if glob.glob("/opt/rocm/include/hip/hip_runtime.h"):
        define_macros.append(("RD_USE_ROCM", "1"))
        libraries += ["amdhip64"]
        print("[setup] ROCm detected — building with AMD HIP backend")
    elif glob.glob("/usr/local/cuda/include/cuda_runtime.h") or glob.glob("/usr/include/cuda_runtime.h"):
        define_macros.append(("RD_USE_CUDA", "1"))
        libraries += ["cudart"]
        print("[setup] CUDA detected — building with NVIDIA CUDA backend")
    else:
        print("[setup] No CUDA/ROCm detected — building CPU fallback (benchmarks still run)")

ext = Extension(
    "red_daft_vram",
    sources=[
        "src/red_daft_vram.cpp",
        "src/red_daft_lora_manager.cpp",
        "src/pybind_wrapper.cpp",
        "src/lora_pybind_wrapper.cpp",
    ],
    include_dirs=[
        "include",
        include_pybind,
    ],
    language="c++",
    extra_compile_args=extra_compile_args,
    define_macros=define_macros,
    libraries=libraries,
)

setup(
    name="red-daft-vram",
    version="1.0.0",
    description="Red Daft OS — 10-pool tiered VRAM Management Engine (CUDA/HIP HAL, async prefetch, emergency borrowing)",
    long_description=open("README.md").read() if pathlib.Path("README.md").exists() else "",
    long_description_content_type="text/markdown",
    ext_modules=[ext],
    zip_safe=False,
    python_requires=">=3.8",
    install_requires=["pybind11>=2.10"],
)
