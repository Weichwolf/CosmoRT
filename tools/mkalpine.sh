#!/bin/sh
# Build ext2 image containing Alpine Linux minirootfs
# Usage: sh tools/mkalpine.sh [ALPINE_ROOT]
#
# Input:  /tmp/alpine-root/ (or $1)
# Output: build/alpine.img (raw ext2 image)
#         build/disk.img   (GPT: ESP + ext2)

set -e
cd "$(dirname "$0")/.."

ALPINE_ROOT="${1:-/tmp/alpine-root}"
IMG=build/disk.img
ESP_MB=64
FS_MB=512
EXT2_TMP=build/alpine.img
EFI_BIN=build/BOOTX64.EFI

if [ ! -d "$ALPINE_ROOT" ]; then
    echo "ERROR: $ALPINE_ROOT not found" >&2
    echo "Download: wget -qO- https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.3-x86_64.tar.gz | tar xzf - -C /tmp/alpine-root" >&2
    exit 1
fi

mkdir -p build

# ── Step 1: Create ext2 image with Alpine rootfs ──
echo "mkalpine: creating ext2 ($FS_MB MB) from $ALPINE_ROOT"
dd if=/dev/zero of="$EXT2_TMP" bs=1M count="$FS_MB" 2>/dev/null
mkfs.ext2 -q -d "$ALPINE_ROOT" "$EXT2_TMP"

# Fix networking (QEMU user-mode)
# ext2fuse or debugfs to inject files after creation
if command -v debugfs >/dev/null 2>&1; then
    echo "nameserver 10.0.2.3" | debugfs -w -R "write /dev/stdin /etc/resolv.conf" "$EXT2_TMP" 2>/dev/null || true
fi

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
           --new=2:${FS_START}:+${FS_SECTORS} --typecode=2:8300 --change-name=2:ext2 \
           "$IMG" >/dev/null
elif command -v parted >/dev/null 2>&1; then
    parted -s "$IMG" mklabel gpt
    parted -s "$IMG" mkpart ESP fat32 "${ESP_START}s" "$((ESP_START + ESP_SECTORS - 1))s"
    parted -s "$IMG" set 1 esp on
    parted -s "$IMG" mkpart ext2 "$((FS_START))s" "$((FS_START + FS_SECTORS - 1))s"
else
    echo "ERROR: need sgdisk or parted for GPT" >&2
    rm -f "$IMG"
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

# ── Step 4: Write ext2 into GPT ──────────────────────
dd if="$EXT2_TMP" of="$IMG" bs=512 seek="$FS_START" conv=notrunc 2>/dev/null

echo "alpine.img: $(du -h "$EXT2_TMP" | cut -f1) ext2"
echo "disk.img:   $(du -h "$IMG" | cut -f1) (GPT: ${ESP_MB}MB ESP + ${FS_MB}MB ext2)"
