#!/usr/bin/env bash
# build-iso.sh — assemble a bootable Red Daft OS live ISO (EFI / GRUB + casper).
# Host deps: mmdebstrap, squashfs-tools, xorriso, grub-efi-<arch>-bin, grub-common
set -euo pipefail

if [[ -n "${RD_ARCH:-}" ]]; then
  case "$RD_ARCH" in
    amd64|x86_64) DEBARCH=amd64; GRUBPKG=grub-efi-amd64-bin; MIRROR="http://archive.ubuntu.com/ubuntu";;
    arm64|aarch64) DEBARCH=arm64; GRUBPKG=grub-efi-arm64-bin; MIRROR="http://ports.ubuntu.com/ubuntu-ports";;
    *) echo "unsupported RD_ARCH: $RD_ARCH"; exit 1;;
  esac
else
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64)  DEBARCH=amd64; GRUBPKG=grub-efi-amd64-bin; MIRROR="http://archive.ubuntu.com/ubuntu";;
    aarch64) DEBARCH=arm64; GRUBPKG=grub-efi-arm64-bin; MIRROR="http://ports.ubuntu.com/ubuntu-ports";;
    *) echo "unsupported arch: $ARCH"; exit 1;;
  esac
fi

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
    --include="linux-image-generic,casper,systemd,systemd-sysv,$GRUBPKG,grub-pc-bin,network-manager,sudo,locales,rsync,parted,xubuntu-core,lightdm,lightdm-gtk-greeter,xfce4-terminal,arc-theme,papirus-icon-theme,git,curl,wget,plymouth,plymouth-label,zstd" \
    --keyring="$KEYRING" \
    noble "$ROOTFS" \
    "deb $MIRROR noble main restricted universe" \
    "deb $MIRROR noble-updates main restricted universe" \
    "deb $MIRROR noble-security main restricted universe"
}

stage_configure() {
  echo "[*] installing Red Daft components"
  mkdir -p "$ROOTFS/opt/daft"
  cp -r packages "$ROOTFS/opt/daft/packages"
  cp -r ai "$ROOTFS/opt/daft/ai"
  cp -r shell "$ROOTFS/opt/daft/shell"
  cp -r ux "$ROOTFS/opt/daft/ux"
  cp -r kernel "$ROOTFS/opt/daft/kernel"
  # Create operator account early so branding chowns succeed.
  chroot "$ROOTFS" useradd -ms /bin/bash -G sudo agent 2>/dev/null || true
  chroot "$ROOTFS" bash /opt/daft/ai/ollama-setup.sh bootstrap 2>/dev/null || echo "[!] ollama bootstrap skipped (no network in chroot)"

  echo "[*] first-boot ID card service"
  install -Dm644 ux/daft-firstrun.service "$ROOTFS/etc/systemd/system/daft-firstrun.service"
  chroot "$ROOTFS" systemctl enable daft-firstrun.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-firstrun.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-firstrun.service"

  echo "[*] brand the OS: os-release, drop Ubiquity, crimson XFCE, welcome terminal"
  chroot "$ROOTFS" bash -c '
    # Remove stock Ubuntu installer if it snuck in
    apt-get purge -y ubiquity ubiquity-slideshow-ubuntu 2>/dev/null || true
    # Brand os-release
    cat > /etc/os-release <<OSR
NAME="Red Daft OS"
VERSION="0.1 (Crimson)"
ID=reddaft
ID_LIKE=ubuntu
PRETTY_NAME="Red Daft OS 0.1"
VERSION_ID="0.1"
HOME_URL="https://github.com/wippsanrinthailand80-commits/red-daft-os"
OSR
    # Crimson XFCE desktop for the agent user
    mkdir -p /home/agent/.config/xfce4/xfconf/xfce-perchannel-xml
    cat > /home/agent/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml <<XML
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor0" type="empty">
        <property name="workspace0" type="empty">
          <property name="image-style" type="int" value="5"/>
          <property name="last-image" type="string" value="/usr/share/backgrounds/reddaft.png"/>
          <property name="color-style" type="int" value="0"/>
          <property name="color1" type="array" value="65535;0;13056;65535"/>
        </property>
      </property>
    </property>
  </property>
</channel>
XML
    chown -R agent:agent /home/agent/.config
    # LightDM greeter crimson background + hide other users
    mkdir -p /etc/lightdm/lightdm.conf.d
    cat >> /etc/lightdm/lightdm.conf.d/10-reddaft.conf <<LDM
[greeter]
background=#0C0C0C
theme-name=Red-Daft
LDM
  '

  # Welcome terminal autostart (outer level to avoid nested single-quote issues)
  cat > "$ROOTFS/usr/local/bin/daft-welcome" <<'WELCOME'
#!/bin/bash
/usr/local/bin/daft-motd
exec bash
WELCOME
  chmod +x "$ROOTFS/usr/local/bin/daft-welcome"
  cat > "$ROOTFS/etc/xdg/autostart/daft-welcome.desktop" <<'DW'
[Desktop Entry]
Name=Red Daft Welcome
Comment=Red Daft OS terminal welcome
Exec=xfce4-terminal -e /usr/local/bin/daft-welcome
Terminal=false
Type=Application
Categories=System;
X-GNOME-Autostart-enabled=true
DW

  echo "[*] crimson wallpaper asset"
  install -Dm644 build/assets/reddaft-wallpaper.png "$ROOTFS/usr/share/backgrounds/reddaft.png"

  echo "[*] XFCE dark/crimson theme (Arc-Dark + Papirus) for agent"
  local XC="$ROOTFS/home/agent/.config/xfce4/xfconf/xfce-perchannel-xml"
  mkdir -p "$XC"
  cat > "$XC/xsettings.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xsettings" version="1.0">
  <property name="Net" type="empty">
    <property name="ThemeName" type="string" value="Arc-Dark"/>
    <property name="IconThemeName" type="string" value="Papirus"/>
  </property>
  <property name="Gtk" type="empty">
    <property name="FontName" type="string" value="Sans 11"/>
    <property name="MonospaceFontName" type="string" value="Monospace 11"/>
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

  echo "[*] load defensive LKM (daft-defmon) at boot"
  install -Dm755 kernel/daft-defmon/detect-hidden.sh "$ROOTFS/usr/local/bin/daft-defmon-detect" 2>/dev/null || true
  echo "daft-defmon" > "$ROOTFS/etc/modules-load.d/daft-defmon.conf"

  echo "[*] AI guard proxy (guarded Ollama on :11435) enabled"
  install -Dm755 ai/daft-ai-guard-proxy.py "$ROOTFS/usr/local/bin/daft-ai-guard-proxy.py"
  install -Dm644 ai/daft-ai-guard.service "$ROOTFS/etc/systemd/system/daft-ai-guard.service"
  chroot "$ROOTFS" systemctl enable daft-ai-guard.service 2>/dev/null || \
    ln -sf /etc/systemd/system/daft-ai-guard.service \
          "$ROOTFS/etc/systemd/system/multi-user.target.wants/daft-ai-guard.service"

  echo "[*] desktop launchers (installer + ID card)"
  install -Dm644 ux/daft-idcard.desktop "$ROOTFS/usr/share/applications/daft-idcard.desktop"

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
  local kv; kv="$(ls -d "$ROOTFS/lib/modules"/*/ 2>/dev/null | head -1 || true)"
  kv="${kv%/}"; kv="${kv##*/}"
  if [[ -n "$kh" ]]; then
    ( cd kernel/daft-defmon && make KDIR="$kh" ) && \
      install -Dm644 kernel/daft-defmon/daft-defmon.ko "$ROOTFS/lib/modules/$kv/extra/daft-defmon.ko" \
      && chroot "$ROOTFS" depmod -a "$kv" \
      || echo "[!] LKM build/install skipped"
  else
    echo "[!] no kernel headers in rootfs; LKM not built"
  fi
}

stage_initramfs() {
  echo "[*] ensuring initramfs exists (casper hooks)"
  chroot "$ROOTFS" bash -c 'update-initramfs -u -k all 2>/dev/null || update-initramfs -c -k all 2>/dev/null' || true
  ls "$ROOTFS/boot"/initrd.img-* >/dev/null 2>&1 || echo "[!] no initrd produced"
}

gen_assets() {
  echo "[*] generating brand assets"
  python3 "$(pwd)/build/gen-assets.py" "$(pwd)/build/assets"
}

stage_iso() {
  echo "[*] assembling live ISO via GRUB + casper"
  mkdir -p "$ISO_SRC/casper" "$ISO_SRC/boot/grub"
  cp build/assets/reddaft-bg.png "$ISO_SRC/boot/grub/reddaft-bg.png"
  local kv; kv="$(ls "$ROOTFS/boot"/vmlinuz-* | head -1)"
  local ir; ir="$(ls "$ROOTFS/boot"/initrd.img-* | head -1)"
  cp "$kv" "$ISO_SRC/casper/vmlinuz"
  cp "$ir" "$ISO_SRC/casper/initrd.img"
  mksquashfs "$ROOTFS" "$ISO_SRC/casper/filesystem.squashfs" -comp zstd
  cat > "$ISO_SRC/boot/grub/grub.cfg" <<'EOF'
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
menuentry "Red Daft OS 0.1 (Live)" {
  linux /casper/vmlinuz boot=casper quiet splash
  initrd /casper/initrd.img
}
menuentry "Red Daft OS 0.1 (Live, safe graphics)" {
  linux /casper/vmlinuz boot=casper quiet splash nomodeset
  initrd /casper/initrd.img
}
EOF
  mkdir -p "$(pwd)/iso"
  grub-mkrescue -o "$ISO_OUT" "$ISO_SRC"
  echo "[+] ISO: $ISO_OUT ($(du -h "$ISO_OUT" | cut -f1))"
}

mkdir -p "$WORK" "$(pwd)/iso"
stage_bootstrap
gen_assets
stage_configure
stage_kernel
stage_initramfs
stage_iso
