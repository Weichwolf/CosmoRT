#!/bin/sh
# Build CosmoFS image containing Alpine Linux minirootfs
# Usage: sh tools/mkalpine.sh [ALPINE_ROOT]
#
# Input:  /tmp/alpine-root/ (or $1)
# Output: build/alpine.img (raw CosmoFS image)
#         build/disk.img   (GPT: ESP + CosmoFS)

set -e
cd "$(dirname "$0")/.."

ALPINE_ROOT="${1:-/tmp/alpine-root}"
IMG=build/disk.img
ESP_MB=64
FS_MB=512
COSMOFS_TMP=.cosmofs.tmp
EFI_BIN=build/BOOTX64.EFI

if [ ! -d "$ALPINE_ROOT" ]; then
    echo "ERROR: $ALPINE_ROOT not found" >&2
    echo "Download: wget -qO- https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.3-x86_64.tar.gz | tar xzf - -C /tmp/alpine-root" >&2
    exit 1
fi

# Build host tools
make tools/mkfs tools/cosmocp 2>/dev/null || {
    gcc -Wall -Wextra -O2 -o tools/mkfs tools/mkfs.c
    gcc -Wall -Wextra -O2 -o tools/cosmocp tools/cosmocp.c
}

# ── Step 1: Create CosmoFS image with Alpine rootfs ──
echo "mkalpine: creating CosmoFS ($FS_MB MB) from $ALPINE_ROOT"
./tools/mkfs "$COSMOFS_TMP" "$FS_MB"

# Copy entire Alpine rootfs (files, directories, symlinks preserved by --tree)
./tools/cosmocp "$COSMOFS_TMP" --tree "$ALPINE_ROOT" /

# Ensure /etc/passwd and /etc/group exist (Alpine minirootfs may have them)
if ! ./tools/cosmocp "$COSMOFS_TMP" --mkdir /etc 2>/dev/null; then true; fi

# Networking (QEMU user-mode)
./tools/cosmocp "$COSMOFS_TMP" --write-string "nameserver 10.0.2.3" /etc/resolv.conf

# ── Step 2: Build GPT disk image ────────────────────
ESP_START=2048
ESP_SECTORS=$((ESP_MB * 2048))
FS_START=$((ESP_START + ESP_SECTORS))
FS_SECTORS=$((FS_MB * 2048))
TOTAL_SECTORS=$((FS_START + FS_SECTORS + 34))

dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" 2>/dev/null

if command -v sgdisk >/dev/null 2>&1; then
    sgdisk --clear \
           --new=1:${ESP_START}:+${ESP_SECTORS} --typecode=1:EF00 --change-name=1:ESP \
           --new=2:${FS_START}:+${FS_SECTORS} --typecode=2:8300 --change-name=2:CosmoFS \
           "$IMG" >/dev/null
elif command -v parted >/dev/null 2>&1; then
    parted -s "$IMG" mklabel gpt
    parted -s "$IMG" mkpart ESP fat32 "${ESP_START}s" "$((ESP_START + ESP_SECTORS - 1))s"
    parted -s "$IMG" set 1 esp on
    parted -s "$IMG" mkpart CosmoFS "$((FS_START))s" "$((FS_START + FS_SECTORS - 1))s"
else
    echo "ERROR: need sgdisk or parted for GPT" >&2
    rm -f "$IMG" "$COSMOFS_TMP"
    exit 1
fi

# ── Step 3: Format ESP (FAT32) ──────────────────────
dd if=/dev/zero of=.esp.tmp bs=512 count="$ESP_SECTORS" 2>/dev/null
mkfs.fat -F 32 .esp.tmp >/dev/null

if [ -f "$EFI_BIN" ]; then
    mmd -i .esp.tmp ::/EFI
    mmd -i .esp.tmp ::/EFI/BOOT
    mcopy -i .esp.tmp "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
else
    echo "WARNING: $EFI_BIN not found (run make first)"
fi

dd if=.esp.tmp of="$IMG" bs=512 seek="$ESP_START" conv=notrunc 2>/dev/null
rm -f .esp.tmp

# ── Step 4: Write CosmoFS into GPT ──────────────────
dd if="$COSMOFS_TMP" of="$IMG" bs=512 seek="$FS_START" conv=notrunc 2>/dev/null
cp "$COSMOFS_TMP" build/alpine.img
rm -f "$COSMOFS_TMP"

echo "alpine.img: $(du -h build/alpine.img | cut -f1) CosmoFS"
echo "disk.img:   $(du -h "$IMG" | cut -f1) (GPT: ${ESP_MB}MB ESP + ${FS_MB}MB CosmoFS)"
