#!/usr/bin/env bash
# check-compat.sh — Red Daft OS GPU compatibility scorer.
#
# Scores a target system (live OS or a staged rootfs tree) against a fixed
# 100-point capability matrix covering the cards Red Daft targets:
#   RTX 3090/4090/5090, RX 7900 XTX / RDNA2-3, MI200/MI300.
# Works with zero hardware present: kernel options and userspace stacks are
# scored from files, not probes.
#
# Matrix (weights):
#   KERNEL   35  DRM/amdgpu/HSA(KFD), HMM+ZONE_DEVICE (UVM-class), IOMMU,
#                THP/PASID/PRI, radeon+nouveau fallbacks, virtio-gpu (CI)
#   FIRMWARE 10  amdgpu DCN firmware, NVIDIA GSP/firmware dir
#   FALLBACK 25  sw GL (llvmpipe), lavapipe Vulkan, PoCL OpenCL, tools
#   ROCm     15  libhsa-runtime, rocminfo/smi, hipcc, rocblas
#   CUDA     15  nvcc, cudart, nvrtc, cudnn
#
# Usage:
#   check-compat.sh [ROOTFS_DIR|/] [--config-only FILE] [--min N] [--json]
# Exit code is nonzero when the score is below --min (default 60).
set -uo pipefail

MIN="${MIN:-60}"
CONFIG_ONLY=""
JSON=0
ROOT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config-only) CONFIG_ONLY="$2"; shift 2 ;;
    --min) MIN="$2"; shift 2 ;;
    --json) JSON=1; shift ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) ROOT="$1"; shift ;;
  esac
done
[[ -z "$ROOT" ]] && ROOT="/"

score=0
declare -A SECT=( [KERNEL]=0 [FIRMWARE]=0 [FALLBACK]=0 [ROCM]=0 [CUDA]=0 )
declare -a LINES

# item <section> <points> <label> <test...>
item() {
  local sect="$1" pts="$2" label="$3"; shift 3
  local got=0
  if "$@" >/dev/null 2>&1; then got=$pts; score=$((score+got)); fi
  SECT[$sect]=$(( ${SECT[$sect]} + got ))
  LINES+=("$(printf '%-8s %3d/%-3d %s' "$sect" "$got" "$pts" "$label")")
}

cfg() {  # cfg <option> <acceptable regex>  — reads config from $CFG_FILE
  grep -E "^${1}=" "$CFG_FILE" 2>/dev/null | head -1 | grep -qE "=${2}$"
}

dir_has_files() { [[ -d "$1" ]] && compgen -G "$1/*" >/dev/null; }
root_glob() { compgen -G "${ROOT%/}/$1" >/dev/null; }
glob_any() { local p; for p in "$@"; do root_glob "$p" && return 0; done; return 1; }

if [[ -n "$CONFIG_ONLY" ]]; then
  # Kernel-section audit of a specific .config file (used by CI kconfig job).
  CFG_FILE="$CONFIG_ONLY"
  [[ -r "$CFG_FILE" ]] || { echo "FATAL: cannot read config '$CFG_FILE'" >&2; exit 2; }

  item KERNEL  2 "DRM core enabled"                 cfg CONFIG_DRM 'y|m'
  item KERNEL  7 "amdgpu (RDNA/CDNA, RX 7000+/MI)"  cfg CONFIG_DRM_AMDGPU 'y|m'
  item KERNEL  6 "HSA/KFD — required by ROCm"       cfg CONFIG_HSA_AMD 'y|m'
  item KERNEL  3 "radeon legacy fallback"           cfg CONFIG_DRM_RADEON 'y|m'
  item KERNEL  3 "nouveau pre-Turing fallback"      cfg CONFIG_DRM_NOUVEAU 'y|m'
  item KERNEL  4 "HMM mirror (UVM-class migration)" cfg CONFIG_HMM_MIRROR 'y'
  item KERNEL  1 "MMU notifier"                     cfg CONFIG_MMU_NOTIFIER 'y|m'
  item KERNEL  3 "ZONE_DEVICE device memory"        cfg CONFIG_ZONE_DEVICE 'y'
  item KERNEL  2 "virtio-gpu (QEMU CI path)"        cfg CONFIG_DRM_VIRTIO_GPU 'y|m'
  item KERNEL  2 "IOMMU support"                    cfg CONFIG_IOMMU_SUPPORT 'y'
  item KERNEL  2 "THP (GPUVM-friendly)"             cfg CONFIG_TRANSPARENT_HUGEPAGE 'y'

  total=$score
  if (( JSON )); then
    echo "{\"kernel\":$total,\"max\":35}"
  else
    echo "== Kernel GPU-readiness: $total/35 =="
    printf '%s\n' "${LINES[@]}"
    echo "threshold: $MIN"
  fi
  (( total >= MIN )) && exit 0
  echo "FAIL: kernel readiness $total < $MIN" >&2
  exit 1
fi

# ── Full-system mode ────────────────────────────────────────────────────
# Kernel config source: shipped /boot/config-$VER in the rootfs, else live.
CFG_FILE=""
for c in "$ROOT"/boot/config-* "$ROOT"/proc/config.gz; do
  [[ -r "$c" ]] && CFG_FILE="$c" && break
done
if [[ "$CFG_FILE" == *.gz ]]; then
  CFG_CMD=(bash -c 'gzip -dc "'"$CFG_FILE"'"')
else
  CFG_CMD=(cat "${CFG_FILE:-/dev/null}")
fi
kopt() { "${CFG_CMD[@]}" 2>/dev/null | grep -E "^${1}=" | head -1 | grep -qE "=${2}$"; }
have_cfg() { [[ -n "$CFG_FILE" ]]; }

if have_cfg; then
  item KERNEL  2 "DRM core enabled"                 kopt CONFIG_DRM 'y|m'
  item KERNEL  7 "amdgpu (RDNA/CDNA, RX 7000+/MI)"  kopt CONFIG_DRM_AMDGPU 'y|m'
  item KERNEL  6 "HSA/KFD — required by ROCm"       kopt CONFIG_HSA_AMD 'y|m'
  item KERNEL  3 "radeon legacy fallback"           kopt CONFIG_DRM_RADEON 'y|m'
  item KERNEL  3 "nouveau pre-Turing fallback"      kopt CONFIG_DRM_NOUVEAU 'y|m'
  item KERNEL  4 "HMM mirror (UVM-class migration)" kopt CONFIG_HMM_MIRROR 'y'
  item KERNEL  1 "MMU notifier"                     kopt CONFIG_MMU_NOTIFIER 'y|m'
  item KERNEL  3 "ZONE_DEVICE device memory"        kopt CONFIG_ZONE_DEVICE 'y'
  item KERNEL  2 "virtio-gpu (QEMU CI path)"        kopt CONFIG_DRM_VIRTIO_GPU 'y|m'
  item KERNEL  2 "IOMMU support"                    kopt CONFIG_IOMMU_SUPPORT 'y'
  item KERNEL  2 "THP (GPUVM-friendly)"             kopt CONFIG_TRANSPARENT_HUGEPAGE 'y'
else
  LINES+=("KERNEL   --- no config found ($ROOT/boot/config-*, /proc/config.gz)")
fi

item FIRMWARE 5 "amdgpu/RDNA firmware"            dir_has_files "$ROOT/lib/firmware/amdgpu"
item FIRMWARE 5 "NVIDIA firmware (incl. GSP)"     dir_has_files "$ROOT/lib/firmware/nvidia"

item FALLBACK 8  "software GL (llvmpipe/swrast)"  glob_any "usr/lib/*/dri/swrast*" "usr/lib/*/dri/llvmpipe*" "usr/lib/*/dri/kms_swrast*" "usr/lib/x86_64-linux-gnu/libgallium*"
item FALLBACK 6  "lavapipe software Vulkan ICD"   root_glob "usr/share/vulkan/icd.d/*lvp*"
item FALLBACK 2  "Vulkan loader"                  root_glob "usr/lib/x86_64-linux-gnu/libvulkan.so*"
item FALLBACK 2  "OpenCL ICD loader"              root_glob "usr/lib/x86_64-linux-gnu/libOpenCL.so*"
item FALLBACK 4  "PoCL CPU OpenCL ICD"            root_glob "etc/OpenCL/vendors/*pocl*"
item FALLBACK 1  "clinfo tool"                    root_glob "usr/bin/clinfo"
item FALLBACK 2  "vulkaninfo tool"                root_glob "usr/bin/vulkaninfo"

item ROCM 7 "libhsa-runtime (KFD userland)"       root_glob "opt/rocm*/lib/libhsa-runtime64.so*"
item ROCM 3 "rocminfo / rocm-smi / amd-smi"       glob_any "opt/rocm*/bin/rocminfo" "opt/rocm*/bin/rocm-smi" "opt/rocm*/bin/amd-smi"
item ROCM 3 "hipcc compiler"                      root_glob "opt/rocm*/bin/hipcc*"
item ROCM 2 "rocblas library"                     root_glob "opt/rocm*/lib/librocblas.so*"

item CUDA 8 "nvcc compiler"                       root_glob "usr/local/cuda*/bin/nvcc"
item CUDA 3 "cudart runtime"                      root_glob "usr/local/cuda*/lib64/libcudart.so*"
item CUDA 2 "nvrtc"                               root_glob "usr/local/cuda*/lib64/libnvrtc.so*"
item CUDA 2 "cuDNN"                               root_glob "usr/local/cuda*/lib64/libcudnn.so*"

total=$score
if (( JSON )); then
  printf '{"kernel":%d,"firmware":%d,"fallback":%d,"rocm":%d,"cuda":%d,"total":%d,"max":100}\n' \
    "${SECT[KERNEL]}" "${SECT[FIRMWARE]}" "${SECT[FALLBACK]}" "${SECT[ROCM]}" "${SECT[CUDA]}" "$total"
else
  echo "== Red Daft GPU compatibility: $total/100 (threshold $MIN) =="
  printf '%s\n' "${LINES[@]}"
  echo "sections: KERNEL ${SECT[KERNEL]}/35  FIRMWARE ${SECT[FIRMWARE]}/10  FALLBACK ${SECT[FALLBACK]}/25  ROCM ${SECT[ROCM]}/15  CUDA ${SECT[CUDA]}/15"
fi

(( total >= MIN )) && exit 0
echo "FAIL: GPU compatibility $total < $MIN" >&2
exit 1
