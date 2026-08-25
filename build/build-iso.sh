#!/usr/bin/env bash
# build-iso.sh — assemble a bootable Red Daft OS live ISO.
# Architecture: kernel.org 7.1.10 compiled from source + Debian bookworm rootfs.
# Host deps: mmdebstrap, squashfs-tools, xorriso, grub-efi-amd64-bin, grub-pc-bin,
#            make, gcc, flex, bison, libssl-dev, libelf-dev, bc, cpio
set -euo pipefail

DEBARCH="${DEBARCH:-amd64}"
GRUBPKG="grub-efi-amd64-bin"
MIRROR="http://deb.debian.org/debian"
SECURITY="http://deb.debian.org/debian-security"
KEYRING="/usr/share/keyrings/debian-archive-keyring.gpg"
KERNEL_VER="${KERNEL_VER:-7.1.10}"

WORK="$(pwd)/build/work"
ROOTFS="$WORK/rootfs"
ISO_OUT="$(pwd)/iso/red-daft-os-$(date +%Y%m%d)-${KERNEL_VER}-$DEBARCH.iso"
ISO_SRC="$WORK/iso-src"
KERNEL_OUT="$WORK/kernel-out"

# Debian archive signing key (for mmdebstrap). Prefer system keyring, else fetch.
if [[ -s /usr/share/keyrings/debian-archive-keyring.gpg ]]; then
  KEYRING="/usr/share/keyrings/debian-archive-keyring.gpg"
elif [[ -s "$(pwd)/build/debian-archive-keyring.gpg" ]]; then
  KEYRING="$(pwd)/build/debian-archive-keyring.gpg"
else
  echo "[*] fetching Debian archive signing key"
  curl -fsSL "https://ftp.debian.org/debian/project/archive.key" -o /tmp/deb-key.asc
  gpg --dearmor -o "$(pwd)/build/debian-archive-keyring.gpg" /tmp/deb-key.asc
  KEYRING="$(pwd)/build/debian-archive-keyring.gpg"
fi
echo "[*] using keyring: $KEYRING"

# Preserve $WORK on failure (post-mortem) or when KEEP_WORK=1 (incremental
# builds: stage_kernel_build then skips straight to the cached vmlinuz).
KEEP_WORK="${KEEP_WORK:-0}"
clean() {
  local rc=$?
  if [[ "$rc" -ne 0 || "$KEEP_WORK" == "1" ]]; then
    echo "[*] keeping $WORK (exit=$rc, KEEP_WORK=$KEEP_WORK)"
  else
    rm -rf "$WORK"
  fi
}
trap clean EXIT

# ── Stage 0: build kernel from source ──────────────────────────────────
stage_kernel_build() {
  if [[ -f "$KERNEL_OUT/boot/vmlinuz-${KERNEL_VER}" ]]; then
    echo "[*] kernel ${KERNEL_VER} already built — skipping"
    return
  fi
  echo "[*] building kernel ${KERNEL_VER} from kernel.org source"
  bash "$(pwd)/build/build-kernel.sh"
  # Build daft-defmon LKM against the just-built kernel build dir.
  echo "[*] building daft-defmon LKM"
  ( cd kernel/daft-defmon && make KDIR="$WORK/kernel-build/build" clean ) 2>/dev/null || true
  ( cd kernel/daft-defmon && make KDIR="$WORK/kernel-build/build" ) && \
    install -Dm644 kernel/daft-defmon/daft-defmon.ko \
      "$KERNEL_OUT/lib/modules/${KERNEL_VER}/extra/daft-defmon.ko" \
    || echo "[!] LKM build skipped"
}

# ── Stage 0b: build the AI-Kernel (second, experimental kernel) ────────
stage_ai_kernel() {
  local AIDIR="$(pwd)/kernel/ai-kernel"
  [[ -d "$AIDIR" ]] || { echo "[!] no ai-kernel sources — skipping"; return; }
  echo "[*] building AI-Kernel (bare-metal Multiboot2)"
  make -C "$AIDIR" clean >/dev/null
  if make -C "$AIDIR" >/dev/null; then
    install -Dm755 "$AIDIR/build/ai-kernel.elf" \
      "$KERNEL_OUT/boot/ai-kernel.elf"
    echo "[+] ai-kernel.elf: $(du -h "$KERNEL_OUT/boot/ai-kernel.elf" | cut -f1)"
  else
    # Never fail the whole ISO because the experimental kernel hiccuped.
    echo "[!] AI-Kernel build failed — dual-boot entry omitted" >&2
  fi
}

# ── Stage 1: bootstrap Debian bookworm rootfs ─────────────────────────
stage_bootstrap() {
  echo "[*] bootstrapping Debian bookworm ($DEBARCH) -> $ROOTFS"
  mkdir -p "$ROOTFS"

  # Incremental: skip bootstrap when a previous run already populated it
  # (mmdebstrap refuses a non-empty target).
  if [[ -s "$ROOTFS/etc/debian_version" ]]; then
    echo "[*] existing rootfs found — skipping bootstrap"
    return
  fi

  # Packages: full base + desktop + security tooling (Debian equivalents
  # of Ubuntu's casper/xubuntu-core: live-boot + xfce4).
  local PKGS="systemd,systemd-sysv,dbus,udev,kmod,\
sudo,locales,rsync,curl,wget,git,\
network-manager,openssh-client,openssh-server,\
coreutils,findutils,binutils,procps,htop,\
python3,python3-pip,python3-venv,\
build-essential,gcc,g++,make,\
libssl-dev,libelf-dev,bc,flex,bison,\
zstd,xz-utils,cpio,\
grub-pc-bin,grub-common,grub2-common,\
live-boot,live-config,live-tools,initramfs-tools,lvm2,dosfstools,parted,gdisk,\
xfce4,xfce4-goodies,lightdm,lightdm-gtk-greeter,xfce4-terminal,\
arc-theme,papirus-icon-theme,\
mesa-utils,libgl1-mesa-dri,libglx-mesa0,mesa-vulkan-drivers,libvulkan1,vulkan-tools,\
ocl-icd-libopencl1,pocl-opencl-icd,clinfo,\
pciutils,nvtop,\
firmware-linux,firmware-misc-nonfree,\
plymouth,plymouth-themes,\
cryptsetup,mdadm"

  mmdebstrap --variant=minbase --arch="$DEBARCH" \
    --include="$PKGS" \
    --aptopt='Acquire::Check-Valid-Until "false"' \
    --keyring="$KEYRING" \
    bookworm "$ROOTFS" \
    "deb $MIRROR bookworm main contrib non-free non-free-firmware" \
    "deb $MIRROR bookworm-updates main contrib non-free non-free-firmware" \
    "deb $SECURITY bookworm-security main contrib non-free non-free-firmware"
}

# ── Stage 2: install from-source kernel into rootfs ────────────────────
stage_kernel_install() {
  echo "[*] installing kernel ${KERNEL_VER} into rootfs"
  # vmlinuz
  install -Dm644 "$KERNEL_OUT/boot/vmlinuz-${KERNEL_VER}" \
    "$ROOTFS/boot/vmlinuz-${KERNEL_VER}"
  # modules (includes daft-defmon.ko in extra/ if built)
  mkdir -p "$ROOTFS/lib/modules"
  cp -a "$KERNEL_OUT/lib/modules/${KERNEL_VER}" "$ROOTFS/lib/modules/${KERNEL_VER}"
  # second kernel (dual-boot on installed systems too)
  if [[ -f "$KERNEL_OUT/boot/ai-kernel.elf" ]]; then
    install -Dm755 "$KERNEL_OUT/boot/ai-kernel.elf" "$ROOTFS/boot/ai-kernel.elf"
    mkdir -p "$ROOTFS/etc/grub.d"
    cat > "$ROOTFS/etc/grub.d/40-ai-kernel" <<'AIK'
#!/bin/sh
cat <<'EOF'
menuentry "Red Daft AI-Kernel 0.1 — experimental bare-metal HMM" {
  insmod multiboot2
  multiboot2 /boot/ai-kernel.elf
  boot
}
EOF
AIK
    chmod +x "$ROOTFS/etc/grub.d/40-ai-kernel"
  fi
  # Kernel metadata for in-OS module development (uapi headers + config +
  # symbol versions). Full kbuild tree is intentionally NOT shipped: it would
  # bloat the squashfs by hundreds of MB. daft-defmon.ko ships prebuilt.
  mkdir -p "$ROOTFS/usr/src/linux-headers-${KERNEL_VER}"
  cp -a "$KERNEL_OUT/usr/include" "$ROOTFS/usr/src/linux-headers-${KERNEL_VER}/include"
  # Kernel metadata expected by Debian userland. mkinitramfs greps
  # /boot/config-${KERNEL_VER} for CONFIG_RD_* to pick initrd compression;
  # without it update-initramfs fails ("CONFIG_RD_GZIP not supported").
  local KB="$WORK/kernel-build/build"
  [[ -f "$KB/.config" ]] && \
    install -m644 "$KB/.config" "$ROOTFS/boot/config-${KERNEL_VER}"
  [[ -f "$KB/System.map" ]] && \
    install -m644 "$KB/System.map" "$ROOTFS/boot/System.map-${KERNEL_VER}"
  [[ -f "$KB/Module.symvers" ]] && \
    install -m644 "$KB/Module.symvers" "$ROOTFS/usr/src/linux-headers-${KERNEL_VER}/Module.symvers"
  # depmod
  chroot "$ROOTFS" depmod -a "${KERNEL_VER}" 2>/dev/null || true
}

# ── Stage 3: Red Daft branding + services ─────────────────────────────
stage_configure() {
  echo "[*] installing Red Daft components"
  mkdir -p "$ROOTFS/opt/daft"
  cp -r packages "$ROOTFS/opt/daft/packages"
  cp -r ai "$ROOTFS/opt/daft/ai"
  cp -r shell "$ROOTFS/opt/daft/shell"
  cp -r ux "$ROOTFS/opt/daft/ux"
  cp -r kernel "$ROOTFS/opt/daft/kernel"
  rm -rf "$ROOTFS/opt/daft/kernel/ai-kernel/build" 2>/dev/null || true

  # Operator account
  chroot "$ROOTFS" useradd -ms /bin/bash -G sudo agent 2>/dev/null || true

  # First-boot ID card
  install -Dm644 ux/daft-firstrun.service "$ROOTFS/etc/systemd/system/daft-firstrun.service"
  chroot "$ROOTFS" systemctl enable daft-firstrun.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-firstrun.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-firstrun.service"

  # Brand os-release
  chroot "$ROOTFS" bash -c 'cat > /etc/os-release <<OSR
NAME="Red Daft OS"
VERSION="0.2 (Crimson)"
ID=reddaft
ID_LIKE=debian
PRETTY_NAME="Red Daft OS 0.2"
VERSION_ID="0.2"
HOME_URL="https://github.com/wippsanrinthailand80-commits/red-daft-os"
OSR'

  # Agent user sudo
  chroot "$ROOTFS" bash -c '
    echo "agent ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/agent
    mkdir -p /etc/systemd/system/getty@tty1.service.d
    printf "[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin agent --noclear %%I $TERM\n" \
      > /etc/systemd/system/getty@tty1.service.d/autologin.conf
  '

  # XFCE crimson desktop
  local XC="$ROOTFS/home/agent/.config/xfce4/xfconf/xfce-perchannel-xml"
  mkdir -p "$XC"
  cat > "$XC/xfce4-desktop.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor0" type="empty">
        <property name="workspace0" type="empty">
          <property name="image-style" type="int" value="5"/>
          <property name="last-image" type="string" value="/usr/share/backgrounds/reddaft.png"/>
        </property>
      </property>
    </property>
  </property>
</channel>
XML
  cat > "$XC/xsettings.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xsettings" version="1.0">
  <property name="Net" type="empty">
    <property name="ThemeName" type="string" value="Arc-Dark"/>
    <property name="IconThemeName" type="string" value="Papirus"/>
  </property>
</channel>
XML
  cat > "$XC/xfwm4.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="theme" type="string" value="Arc"/>
  </property>
</channel>
XML
  chroot "$ROOTFS" chown -R agent:agent /home/agent/.config

  # Wallpaper
  install -Dm644 build/assets/reddaft-wallpaper.png "$ROOTFS/usr/share/backgrounds/reddaft.png"

  # LightDM autologin
  mkdir -p "$ROOTFS/etc/lightdm/lightdm.conf.d"
  cat > "$ROOTFS/etc/lightdm/lightdm.conf.d/10-reddaft.conf" <<LDM
[Seat:*]
autologin-user=agent
autologin-session=xfce
user-session=xfce
[greeter]
background=#0C0C0C
theme-name=Red-Daft
LDM

  # Welcome terminal
  cat > "$ROOTFS/usr/local/bin/daft-welcome" <<'WELCOME'
#!/bin/bash
/usr/local/bin/daft-motd 2>/dev/null || true
exec bash
WELCOME
  chmod +x "$ROOTFS/usr/local/bin/daft-welcome"
  cat > "$ROOTFS/etc/xdg/autostart/daft-welcome.desktop" <<'DW'
[Desktop Entry]
Name=Red Daft Welcome
Exec=xfce4-terminal -e /usr/local/bin/daft-welcome
Terminal=false
Type=Application
DW

  # Defensive LKM at boot
  install -Dm755 kernel/daft-defmon/detect-hidden.sh "$ROOTFS/usr/local/bin/daft-defmon-detect" 2>/dev/null || true
  echo "daft-defmon" > "$ROOTFS/etc/modules-load.d/daft-defmon.conf"

  # AI guard proxy
  install -Dm755 ai/daft-ai-guard-proxy.py "$ROOTFS/usr/local/bin/daft-ai-guard-proxy.py"
  install -Dm644 ai/daft-ai-guard.service "$ROOTFS/etc/systemd/system/daft-ai-guard.service"
  chroot "$ROOTFS" systemctl enable daft-ai-guard.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-ai-guard.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-ai-guard.service"

  # MOTD
  install -Dm755 ux/daft-motd.sh "$ROOTFS/usr/local/bin/daft-motd" 2>/dev/null || true

  # Plymouth
  mkdir -p "$ROOTFS/usr/share/plymouth/themes/reddaft"
  install -Dm644 build/plymouth/reddaft/reddaft.plymouth "$ROOTFS/usr/share/plymouth/themes/reddaft/reddaft.plymouth"
  install -Dm644 build/plymouth/reddaft/reddaft.script "$ROOTFS/usr/share/plymouth/themes/reddaft/reddaft.script"
  chroot "$ROOTFS" plymouth-set-default-theme reddaft 2>/dev/null || true
  # daft-pkg manager into rootfs
  if [[ -d packages/daft-pkg ]]; then
    cp -r packages/daft-pkg "$ROOTFS/opt/daft/daft-pkg" 2>/dev/null || true
    chroot "$ROOTFS" ln -sf /opt/daft/daft-pkg/daft-pkg.sh /usr/local/bin/daft-pkg 2>/dev/null || true
  fi

  # GPU compatibility layer: detect/activate/fallback + capability scorer.
  # Runs at every boot (daft-gpu.service) so hot-swapped or newly-installed
  # stacks are picked up without user action. Scores via /boot/config-$VER.
  if [[ -d packages/gpu ]]; then
    install -m755 packages/gpu/daft-gpu.sh "$ROOTFS/usr/local/bin/daft-gpu"
    install -m755 packages/gpu/check-compat.sh "$ROOTFS/usr/local/bin/daft-compat"
    install -Dm644 packages/gpu/daft-gpu.service \
      "$ROOTFS/etc/systemd/system/daft-gpu.service"
    chroot "$ROOTFS" systemctl enable daft-gpu.service 2>/dev/null || \
      ln -sf /etc/systemd/system/daft-gpu.service \
            "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-gpu.service"
  fi

  # Desktop launchers
  install -Dm644 ux/daft-idcard.desktop "$ROOTFS/usr/share/applications/daft-idcard.desktop" 2>/dev/null || true
  install -Dm755 ux/reddaft-install.sh "$ROOTFS/usr/local/bin/reddaft-install" 2>/dev/null || true
  cat > "$ROOTFS/usr/share/applications/reddaft-install.desktop" <<'D'
[Desktop Entry]
Name=Red Daft Installer
Exec=sudo /usr/local/bin/reddaft-install
Icon=drive-harddisk
Terminal=true
Type=Application
D

  # ── Initramfs: generate LAST, after plymouth theme + all branding. ────
  # The initramfs bakes in the configured plymouth theme and must see the
  # final module tree, so this cannot happen during kernel install.
  # Loud failure: a missing initrd means an unbootable ISO — never mask it.
  echo "[*] generating initramfs ${KERNEL_VER}"
  if [[ -f "$ROOTFS/boot/initrd.img-${KERNEL_VER}" ]]; then
    chroot "$ROOTFS" update-initramfs -u -k "${KERNEL_VER}"
  else
    chroot "$ROOTFS" update-initramfs -c -k "${KERNEL_VER}"
  fi
  [[ -f "$ROOTFS/boot/initrd.img-${KERNEL_VER}" ]] || {
    echo "[!] FATAL: update-initramfs produced no /boot/initrd.img-${KERNEL_VER}" >&2
    exit 1
  }
}

# ── Stage 5: assemble ISO ─────────────────────────────────────────────
stage_iso() {
  echo "[*] assembling live ISO via GRUB + live-boot (Debian)"
  mkdir -p "$ISO_SRC/live" "$ISO_SRC/boot/grub"
  cp build/assets/reddaft-bg.png "$ISO_SRC/boot/grub/reddaft-bg.png"

  cp "$ROOTFS/boot/vmlinuz-${KERNEL_VER}" "$ISO_SRC/live/vmlinuz"
  cp "$ROOTFS/boot/initrd.img-${KERNEL_VER}" "$ISO_SRC/live/initrd.img"

  # Second kernel: bare-metal AI-Kernel (Multiboot2), if it built.
  local AIK=""
  if [[ -f "$KERNEL_OUT/boot/ai-kernel.elf" ]]; then
    cp "$KERNEL_OUT/boot/ai-kernel.elf" "$ISO_SRC/boot/ai-kernel.elf"
    AIK="yes"
  fi

  mksquashfs "$ROOTFS" "$ISO_SRC/live/filesystem.squashfs" -comp zstd

  cat > "$ISO_SRC/boot/grub/grub.cfg" <<EOF
set timeout=10
set default=0
insmod all_video
insmod gfxterm
insmod png
background_image /boot/grub/reddaft-bg.png
set color_normal=white/black
set color_highlight=red/black
set menu_color_normal=white/black
set menu_color_highlight=red/black
menuentry "Red Daft OS 0.2 — kernel ${KERNEL_VER} (Live)" {
  linux /live/vmlinuz boot=live quiet splash
  initrd /live/initrd.img
}
menuentry "Red Daft OS 0.2 — kernel ${KERNEL_VER} (safe graphics)" {
  linux /live/vmlinuz boot=live quiet splash nomodeset
  initrd /live/initrd.img
}
EOF

  if [[ -n "$AIK" ]]; then
    cat >> "$ISO_SRC/boot/grub/grub.cfg" <<EOF
menuentry "Red Daft AI-Kernel 0.1 — experimental bare-metal HMM" {
  insmod multiboot2
  echo "Loading AI-Kernel (serial console on COM1)..."
  multiboot2 /boot/ai-kernel.elf
  boot
}
EOF
  fi

  mkdir -p "$(pwd)/iso"
  grub-mkrescue -o "$ISO_OUT" "$ISO_SRC"
  echo "[+] ISO: $ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
}

# ── Main ───────────────────────────────────────────────────────────────
gen_assets() {
  echo "[*] generating brand assets"
  python3 "$(pwd)/build/gen-assets.py" "$(pwd)/build/assets"
}

mkdir -p "$WORK" "$(pwd)/iso"
stage_kernel_build
stage_ai_kernel
stage_bootstrap
stage_kernel_install
gen_assets
stage_configure
stage_iso
