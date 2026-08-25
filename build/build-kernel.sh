#!/usr/bin/env bash
# build-kernel.sh — download and compile Linux 7.1.10 from kernel.org.
# Produces: vmlinuz + modules under $WORK/kernel/
# Host deps: make, gcc/g++, flex, bison, libssl-dev, libelf-dev, bc, cpio
set -euo pipefail

KERNEL_VER="${KERNEL_VER:-7.1.10}"
KERNEL_MAJOR="${KERNEL_VER%%.*}"
WORK="$(pwd)/build/work/kernel-build"
OUT="$(pwd)/build/work/kernel-out"
SRC_TAR="linux-${KERNEL_VER}.tar.xz"
SRC_URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_MAJOR}.x/${SRC_TAR}"
JOBS="${JOBS:-$(nproc)}"
CUSTOM_CFG="$(pwd)/build/configs/kernel-config.x86_64"

mkdir -p "$WORK" "$OUT" "$(pwd)/iso"

echo "[*] kernel ${KERNEL_VER} — ${JOBS} jobs"

# Download
if [[ ! -f "$WORK/$SRC_TAR" ]]; then
  echo "[*] downloading kernel source"
  curl -fSL --retry 3 "$SRC_URL" -o "$WORK/$SRC_TAR"
fi

# Extract
SRCDIR="$WORK/linux-${KERNEL_VER}"
if [[ ! -d "$SRCDIR" ]]; then
  echo "[*] extracting"
  tar -xf "$WORK/$SRC_TAR" -C "$WORK"
fi

# Configure: start from defconfig, then overlay our hardened options
echo "[*] configuring kernel"
make -C "$SRCDIR" O="$WORK/build" defconfig JOBS="$JOBS" 2>&1 | tail -1
if [[ -s "$CUSTOM_CFG" ]]; then
  echo "[*] merging hardened config overlay"
  cat "$CUSTOM_CFG" >> "$WORK/build/.config"
  make -C "$SRCDIR" O="$WORK/build" olddefconfig 2>&1 | tail -1
fi

# Build
echo "[*] compiling kernel + modules"
make -C "$SRCDIR" O="$WORK/build" -j"$JOBS" bzImage modules 2>&1 | tail -3

# Install into OUT
echo "[*] installing to $OUT"
mkdir -p "$OUT/boot" "$OUT/lib/modules"
cp "$WORK/build/arch/x86/boot/bzImage" "$OUT/boot/vmlinuz-${KERNEL_VER}"
make -C "$SRCDIR" O="$WORK/build" modules_install INSTALL_MOD_PATH="$OUT" 2>&1 | tail -1
make -C "$SRCDIR" O="$WORK/build" headers_install INSTALL_HDR_PATH="$OUT/usr" 2>&1 | tail -1

echo "[+] kernel ${KERNEL_VER} built:"
echo "    vmlinuz: $OUT/boot/vmlinuz-${KERNEL_VER} ($(du -h "$OUT/boot/vmlinuz-${KERNEL_VER}" | cut -f1))"
echo "    modules: $OUT/lib/modules/${KERNEL_VER}/"
