#!/usr/bin/env bash
# build-iso.sh — assemble a bootable Red Daft OS live ISO (EFI / GRUB + casper).
# Host deps: mmdebstrap, squashfs-tools, xorriso, grub-efi-<arch>-bin, grub-common
set -euo pipefail

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64)  DEBARCH=amd64; GRUBPKG=grub-efi-amd64-bin; MIRROR="http://archive.ubuntu.com/ubuntu";;
  aarch64) DEBARCH=arm64; GRUBPKG=grub-efi-arm64-bin; MIRROR="http://ports.ubuntu.com/ubuntu-ports";;
  *) echo "unsupported arch: $ARCH"; exit 1;;
esac

WORK="$(pwd)/build/work"
ROOTFS="$WORK/rootfs"
ISO_OUT="$(pwd)/iso/red-daft-os-$(date +%Y%m%d)-$DEBARCH.iso"
ISO_SRC="$WORK/iso-src"

# Fetch Ubuntu archive signing key so mmdebstrap can verify Release files.
KEYRING="$(pwd)/build/ubuntu-archive-keyring.gpg"
# Prefer the system keyring when present (e.g. on Ubuntu CI runners).
if [[ -s /usr/share/keyrings/ubuntu-archive-keyring.gpg ]]; then
  KEYRING="/usr/share/keyrings/ubuntu-archive-keyring.gpg"
elif [[ ! -s "$KEYRING" ]]; then
  echo "[*] fetching Ubuntu archive signing key"
  curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0xF6ECB3762474EDA9D21B7022871920D1991BC93C" \
    -o build/ubuntu-archive-keyring.pgp
  gpg --dearmor -o "$KEYRING" build/ubuntu-archive-keyring.pgp 2>/dev/null \
    || cp build/ubuntu-archive-keyring.pgp "$KEYRING"
fi

clean() { rm -rf "$WORK"; }
trap clean EXIT

stage_bootstrap() {
  echo "[*] bootstrapping Ubuntu noble ($DEBARCH) -> $ROOTFS"
  mkdir -p "$ROOTFS"
  mmdebstrap --variant=minbase --arch="$DEBARCH" \
    --include="linux-image-generic,casper,systemd,systemd-sysv,$GRUBPKG,network-manager,sudo,locales,rsync,parted,xubuntu-core,lightdm,lightdm-gtk-greeter,xfce4-terminal" \
    --keyring="$KEYRING" \
    noble "$ROOTFS" \
    "deb $MIRROR noble main restricted universe" \
    "deb $MIRROR noble-updates main restricted universe" \
    "deb $MIRROR noble-security main restricted universe"
}

stage_configure() {
  echo "[*] installing Red Daft components"
  cp -r packages ai shell ux kernel "$ROOTFS/opt/daft/" 2>/dev/null || true
  cp -r packages "$ROOTFS/opt/daft/packages"
  cp -r ai "$ROOTFS/opt/daft/ai"
  cp -r shell "$ROOTFS/opt/daft/shell"
  cp -r ux "$ROOTFS/opt/daft/ux"
  cp -r kernel "$ROOTFS/opt/daft/kernel"
  chroot "$ROOTFS" bash /opt/daft/ai/ollama-setup.sh bootstrap 2>/dev/null || echo "[!] ollama bootstrap skipped (no network in chroot)"

  echo "[*] first-boot ID card service"
  install -Dm644 ux/daft-firstrun.service "$ROOTFS/etc/systemd/system/daft-firstrun.service"
  chroot "$ROOTFS" systemctl enable daft-firstrun.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-firstrun.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-firstrun.service"

  echo "[*] MOTD"
  chroot "$ROOTFS" bash /opt/daft/ux/setup-motd.sh / 2>/dev/null || \
    install -Dm755 ux/daft-motd.sh "$ROOTFS/usr/local/bin/daft-motd"

  echo "[*] plymouth theme"
  mkdir -p "$ROOTFS/usr/share/plymouth/themes/reddaft"
  install -Dm644 build/plymouth/reddaft/reddaft.plymouth "$ROOTFS/usr/share/plymouth/themes/reddaft/reddaft.plymouth"
  install -Dm644 build/plymouth/reddaft/reddaft.script  "$ROOTFS/usr/share/plymouth/themes/reddaft/reddaft.script"
  chroot "$ROOTFS" bash -c 'apt-get install -y plymouth plymouth-themes 2>/dev/null; plymouth-set-default-theme reddaft 2>/dev/null; update-initramfs -u 2>/dev/null' || true

  echo "[*] live operator account + getty autologin (so the OS is usable on boot)"
  chroot "$ROOTFS" bash -c '
    useradd -ms /bin/bash -G sudo agent 2>/dev/null || true
    echo "agent ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/agent
    mkdir -p /etc/systemd/system/getty@tty1.service.d
    printf "[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin agent --noclear %%I \$TERM\n" \
      > /etc/systemd/system/getty@tty1.service.d/autologin.conf
  '

  echo "[*] XFCE desktop + lightdm autologin"
  chroot "$ROOTFS" bash -c '
    mkdir -p /etc/lightdm/lightdm.conf.d
    cat > /etc/lightdm/lightdm.conf.d/10-reddaft.conf <<LDM
[Seat:*]
autologin-user=agent
autologin-session=xfce
user-session=xfce
LDM
  '

  echo "[*] disk installer + desktop launcher"
  install -Dm755 ux/reddaft-install.sh "$ROOTFS/usr/local/bin/reddaft-install"
  cat > "$ROOTFS/usr/share/applications/reddaft-install.desktop" <<'D'
[Desktop Entry]
Name=Red Daft Installer
Comment=Install Red Daft OS to a disk
Exec=sudo /usr/local/bin/reddaft-install
Icon=drive-harddisk
Terminal=true
Type=Application
Categories=System;
D
}

stage_kernel() {
  echo "[*] building defensive LKM (daft-defmon)"
  local kh; kh="$(ls -d "$ROOTFS/lib/modules"/*/build 2>/dev/null | head -1 || true)"
  if [[ -n "$kh" ]]; then
    ( cd kernel/daft-defmon && make KDIR="$kh" ) && \
      install -Dm644 kernel/daft-defmon/daft-defmon.ko "$ROOTFS/usr/lib/modules/extra/daft-defmon.ko" \
      || echo "[!] LKM build skipped"
  else
    echo "[!] no kernel headers in rootfs; LKM not built"
  fi
}

stage_initramfs() {
  echo "[*] ensuring initramfs exists (casper hooks)"
  chroot "$ROOTFS" bash -c 'update-initramfs -u -k all 2>/dev/null || update-initramfs -c -k all 2>/dev/null' || true
  ls "$ROOTFS/boot"/initrd.img-* >/dev/null 2>&1 || echo "[!] no initrd produced"
}

stage_iso() {
  echo "[*] assembling live ISO via GRUB + casper"
  mkdir -p "$ISO_SRC/casper" "$ISO_SRC/boot/grub"
  local kv; kv="$(ls "$ROOTFS/boot"/vmlinuz-* | head -1)"
  local ir; ir="$(ls "$ROOTFS/boot"/initrd.img-* | head -1)"
  cp "$kv" "$ISO_SRC/casper/vmlinuz"
  cp "$ir" "$ISO_SRC/casper/initrd.img"
  mksquashfs "$ROOTFS" "$ISO_SRC/casper/filesystem.squashfs" -comp zstd
  cat > "$ISO_SRC/boot/grub/grub.cfg" <<'EOF'
set timeout=10
set default=0
menuentry "Red Daft OS (Live)" {
  linux /casper/vmlinuz boot=casper quiet splash
  initrd /casper/initrd.img
}
EOF
  mkdir -p "$(pwd)/iso"
  grub-mkrescue -o "$ISO_OUT" "$ISO_SRC"
  echo "[+] ISO: $ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
}

mkdir -p "$WORK" "$(pwd)/iso"
stage_bootstrap
stage_configure
stage_kernel
stage_initramfs
stage_iso
