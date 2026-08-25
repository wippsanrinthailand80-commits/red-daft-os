#!/usr/bin/env bash
# reddaft-install.sh — minimal installer: clones the running live Red Daft OS
# to a target disk (GPT + EFI + ext4) and makes it bootable.
# This is a lean, transparent installer (not Ubiquity) — review before use.
set -euo pipefail

echo "============================================="
echo "  RED DAFT OS — disk installer"
echo "============================================="
read -rp "Target disk (e.g. /dev/sda, /dev/vda): " DISK
[[ -b "$DISK" ]] || { echo "error: $DISK is not a block device"; exit 1; }

read -rp "Hostname [reddaft]: " HOST; HOST="${HOST:-reddaft}"
read -rp "This will ERASE all data on $DISK. Type YES to continue: " CONFIRM
[[ "$CONFIRM" == "YES" ]] || { echo "aborted"; exit 1; }

echo "[*] partitioning $DISK (GPT: EFI + root)"
parted -s "$DISK" mklabel gpt
parted -s "$DISK" mkpart ESP fat32 1MiB 513MiB
parted -s "$DISK" set 1 esp on
parted -s "$DISK" mkpart root ext4 513MiB 100%

EFI="${DISK}1"
ROOT="${DISK}2"
mkfs.vfat -F32 "$EFI"
mkfs.ext4 -F "$ROOT"

MNT="$(mktemp -d)"
mount "$ROOT" "$MNT"
mkdir -p "$MNT/boot/efi"
mount "$EFI" "$MNT/boot/efi"

echo "[*] cloning live system to $ROOT (this takes a while)"
rsync -aAXH --exclude=/proc --exclude=/sys --exclude=/dev \
  --exclude=/run --exclude=/mnt --exclude=/media --exclude=/tmp \
  --exclude=/boot/efi --exclude="$MNT" / "$MNT/"

echo "[*] writing fstab + hostname"
UUID_ROOT="$(blkid -s UUID -o value "$ROOT")"
UUID_EFI="$(blkid -s UUID -o value "$EFI")"
cat > "$MNT/etc/fstab" <<F
UUID=$UUID_ROOT /        ext4    defaults        0 1
UUID=$UUID_EFI  /boot/efi vfat   umask=0077       0 2
F
echo "$HOST" > "$MNT/etc/hostname"
echo "127.0.1.1 $HOST" >> "$MNT/etc/hosts"

echo "[*] installing bootloader"
mount --bind /dev "$MNT/dev"
mount --bind /proc "$MNT/proc"
mount --bind /sys "$MNT/sys"
chroot "$MNT" bash -c \
  "apt-get install -y grub-efi-\$(dpkg --print-architecture) >/dev/null 2>&1; grub-install; update-grub" \
  || echo "[!] grub install step needs network or manual fix"
umount -R "$MNT" 2>/dev/null || true

echo "[+] Install complete. Remove the live media and reboot into $HOST."
