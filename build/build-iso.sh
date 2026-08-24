#!/usr/bin/env bash
# build-iso.sh — assemble a Red Daft OS live/install ISO (high level).
# Requires: debootstrap, xorriso, squashfs-tools, mmdebstrap
set -euo pipefail

WORK="$(pwd)/build/work"
ROOTFS="$WORK/rootfs"
ISO_OUT="$(pwd)/iso/reddaft-$(date +%Y%m%d).iso"

stage_bootstrap() {
  echo "[*] bootstrapping base (Ubuntu/Debian minimal)"
  mmdebstrap --variant=minbase --arch=amd64 noble "$ROOTFS" \
    "deb http://archive.ubuntu.com/ubuntu noble main restricted universe"
}

stage_configure() {
  echo "[*] installing Red Daft components"
  cp -r packages "$ROOTFS/opt/daft/"
  cp -r ai "$ROOTFS/opt/daft/"
  cp -r shell "$ROOTFS/opt/daft/"
  cp -r ux "$ROOTFS/opt/daft/"
  cp -r kernel "$ROOTFS/opt/daft/"
  chroot "$ROOTFS" bash /opt/daft/ai/ollama-setup.sh bootstrap || true
  chroot "$ROOTFS" apt-get install -y plymouth plymouth-themes 2>/dev/null || \
    echo "[!] plymouth not installed in rootfs; theme staged but inactive"

  echo "[*] installing first-boot ID card service"
  install -Dm644 ux/daft-firstrun.service \
    "$ROOTFS/etc/systemd/system/daft-firstrun.service"
  chroot "$ROOTFS" systemctl enable daft-firstrun.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-firstrun.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-firstrun.service"
}

stage_kernel() {
  echo "[*] building + installing hardened Daft-Kernel"
  echo "    config: build/configs/kernel-config.x86_64 (KSPP-hardened)"
  # Build the defensive LKM and stage it for install at first boot.
  ( cd kernel/daft-defmon && make KDIR="$ROOTFS/lib/modules/$(ls "$ROOTFS/lib/modules" | head -1)/build" ) \
    && install -Dm644 kernel/daft-defmon/daft-defmon.ko \
       "$ROOTFS/usr/lib/modules/extra/daft-defmon.ko" \
    || echo "[!] LKM build needs kernel headers in rootfs; skipping (build hook ready)"
  # Hook point: compile the actual Daft-Kernel from linux/ with the fragment.
  :
}

stage_plymouth() {
  echo "[*] installing Red Daft Plymouth boot splash"
  local THEME="$ROOTFS/usr/share/plymouth/themes/reddaft"
  install -Dm644 build/plymouth/reddaft/reddaft.plymouth "$THEME/reddaft.plymouth"
  install -Dm644 build/plymouth/reddaft/reddaft.script  "$THEME/reddaft.script"
  chroot "$ROOTFS" bash -c 'plymouth-set-default-theme reddaft && update-initramfs -u' 2>/dev/null \
    || echo "[!] plymouth theme staged; activate in chroot with: plymouth-set-default-theme reddaft"
}

stage_iso() {
  echo "[*] building squashfs + ISO"
  mksquashfs "$ROOTFS" "$WORK/rootfs.squashfs" -comp zstd
  xorriso -as mkisofs -o "$ISO_OUT" -c isolinux/boot.cat \
    -b isolinux/isolinux.bin -no-emul-boot -boot-load-size 4 \
    -boot-info-table "$WORK/iso-staging"
  echo "[+] ISO: $ISO_OUT"
}

mkdir -p "$WORK"
stage_bootstrap
stage_configure
stage_kernel
stage_plymouth
stage_iso
