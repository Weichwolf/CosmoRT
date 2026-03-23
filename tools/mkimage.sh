#!/bin/sh
# Build combined CosmoRT disk image: GPT with ESP + CosmoFS partitions
# Usage: sh tools/mkimage.sh
#
# Output: disk.img (GPT, raw)
#   Partition 1: ESP (FAT32, 64MB)  -> EFI/BOOT/BOOTX64.EFI
#   Partition 2: CosmoFS (512MB)    -> Userland
#
# Convert to VHDX: qemu-img convert -f raw -O vhdx disk.img disk.vhdx

set -e
cd "$(dirname "$0")/.."

PX=~/Git/CosmoPX
BREW="$PX/brew/prefix"
IMG=disk.img
ESP_MB=64
FS_MB=512
TOTAL_MB=$((ESP_MB + FS_MB + 2))  # +2 for GPT headers

EFI_BIN=build/BOOTX64.EFI
COSMOFS_TMP=.cosmofs.tmp

# Build host tools
make tools/mkfs tools/cosmo_cp 2>/dev/null || {
    gcc -Wall -Wextra -O2 -o tools/mkfs tools/mkfs.c
    gcc -Wall -Wextra -O2 -o tools/cosmo_cp tools/cosmo_cp.c
}

# ── Step 1: Create CosmoFS partition image ──────────
./tools/mkfs "$COSMOFS_TMP" "$FS_MB"

# Directories
for d in usr usr/bin home tmp etc opt opt/claude-code dev; do
    ./tools/cosmo_cp "$COSMOFS_TMP" --mkdir "/$d"
done

# Shell + coreutils
if [ -d "$PX/coreutils/build" ]; then
    for bin in "$PX"/coreutils/build/*; do
        ./tools/cosmo_cp "$COSMOFS_TMP" "$bin" "/usr/bin/$(basename "$bin")"
    done
fi
[ -f "$PX/sh/build/sh" ] && ./tools/cosmo_cp "$COSMOFS_TMP" "$PX/sh/build/sh" /usr/bin/sh
[ -f "$BREW/bin/bash" ] && ./tools/cosmo_cp "$COSMOFS_TMP" "$BREW/bin/bash" /usr/bin/bash

# Node.js
[ -f "$BREW/bin/node" ] && ./tools/cosmo_cp "$COSMOFS_TMP" "$BREW/bin/node" /usr/bin/node

# Claude Code
[ -d "$BREW/lib/node_modules/@anthropic-ai/claude-code" ] && \
    ./tools/cosmo_cp "$COSMOFS_TMP" --tree "$BREW/lib/node_modules/@anthropic-ai/claude-code" /opt/claude-code

# /etc
./tools/cosmo_cp "$COSMOFS_TMP" --write-string "nameserver 10.0.2.3" /etc/resolv.conf
./tools/cosmo_cp "$COSMOFS_TMP" --write-string "127.0.0.1 localhost" /etc/hosts

# Boot-test script (init runs /home/.boot if present, else starts bash -i)
if [ -z "$COSMO_INTERACTIVE" ] && [ -f tools/boot-test.sh ]; then
    ./tools/cosmo_cp "$COSMOFS_TMP" tools/boot-test.sh /home/.boot
fi

# ── Step 2: Build combined GPT image ───────────────
ESP_START=2048                              # sector 2048 = 1MB (standard GPT alignment)
ESP_SECTORS=$((ESP_MB * 2048))              # sectors (512 bytes each)
FS_START=$((ESP_START + ESP_SECTORS))
FS_SECTORS=$((FS_MB * 2048))
TOTAL_SECTORS=$((FS_START + FS_SECTORS + 34))  # +34 for backup GPT

# Create raw image
dd if=/dev/zero of="$IMG" bs=512 count="$TOTAL_SECTORS" 2>/dev/null

# Create GPT partition table
if command -v sgdisk >/dev/null 2>&1; then
    # sgdisk available
    sgdisk --clear \
           --new=1:${ESP_START}:+${ESP_SECTORS} --typecode=1:EF00 --change-name=1:ESP \
           --new=2:${FS_START}:+${FS_SECTORS} --typecode=2:8300 --change-name=2:CosmoFS \
           "$IMG" >/dev/null
elif command -v parted >/dev/null 2>&1; then
    # parted fallback
    parted -s "$IMG" mklabel gpt
    parted -s "$IMG" mkpart ESP fat32 "${ESP_START}s" "$((ESP_START + ESP_SECTORS - 1))s"
    parted -s "$IMG" set 1 esp on
    parted -s "$IMG" mkpart CosmoFS "$((FS_START))s" "$((FS_START + FS_SECTORS - 1))s"
else
    echo "ERROR: need sgdisk or parted for GPT" >&2
    rm -f "$IMG" "$COSMOFS_TMP"
    exit 1
fi

# ── Step 3: Format ESP partition (FAT32) ───────────
ESP_OFFSET=$((ESP_START * 512))
ESP_SIZE=$((ESP_SECTORS * 512))
dd if=/dev/zero of=.esp.tmp bs=512 count="$ESP_SECTORS" 2>/dev/null
mkfs.fat -F 32 .esp.tmp >/dev/null

# Copy EFI binary into ESP
if [ -f "$EFI_BIN" ]; then
    mmd -i .esp.tmp ::/EFI
    mmd -i .esp.tmp ::/EFI/BOOT
    mcopy -i .esp.tmp "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
else
    echo "WARNING: $EFI_BIN not found, ESP will be empty (run make first)"
fi

# Write ESP into combined image at partition offset
dd if=.esp.tmp of="$IMG" bs=512 seek="$ESP_START" conv=notrunc 2>/dev/null
rm -f .esp.tmp

# ── Step 4: Write CosmoFS into combined image ─────
FS_OFFSET=$((FS_START * 512))
dd if="$COSMOFS_TMP" of="$IMG" bs=512 seek="$FS_START" conv=notrunc 2>/dev/null
cp "$COSMOFS_TMP" build/cosmofs.img
rm -f "$COSMOFS_TMP"

echo "disk.img: $(du -h "$IMG" | cut -f1) (GPT: ${ESP_MB}MB ESP + ${FS_MB}MB CosmoFS)"
