#!/bin/sh
# Build combined CosmoRT disk image: GPT with ESP + ext2 partitions
# Usage: sh tools/mkimage.sh
#
# Output: disk.img (GPT, raw)
#   Partition 1: ESP (FAT32, 64MB)  -> EFI/BOOT/BOOTX64.EFI
#   Partition 2: ext2 (512MB)       -> Userland
#
# Convert to VHDX: qemu-img convert -f raw -O vhdx disk.img disk.vhdx

set -e
cd "$(dirname "$0")/.."

PX=~/Git/CosmoPX
BREW="$PX/brew/prefix"
IMG=build/disk.img
ESP_MB=64
FS_MB=512
TOTAL_MB=$((ESP_MB + FS_MB + 2))  # +2 for GPT headers

EFI_BIN=build/BOOTX64.EFI
EXT2_TMP=build/root.ext2
ROOTFS_TMP=$(mktemp -d)

mkdir -p build

# ── Step 1: Build rootfs directory ────────────────────
mkdir -p "$ROOTFS_TMP"/{usr/bin,usr/lib,usr/local/ssl,home/.npm,tmp,etc/ssl,opt/claude-code,dev}

# Shell + coreutils
if [ -d "$PX/coreutils/build" ]; then
    for bin in "$PX"/coreutils/build/*; do
        cp "$bin" "$ROOTFS_TMP/usr/bin/$(basename "$bin")"
    done
fi
[ -f "$PX/sh/build/sh" ] && cp "$PX/sh/build/sh" "$ROOTFS_TMP/usr/bin/sh"
[ -f "$BREW/bin/bash" ] && cp "$BREW/bin/bash" "$ROOTFS_TMP/usr/bin/bash"

# Node.js
[ -f "$BREW/bin/node" ] && cp "$BREW/bin/node" "$ROOTFS_TMP/usr/bin/node"

# npm
if [ -d "$BREW/lib/node_modules/npm" ]; then
    mkdir -p "$ROOTFS_TMP/usr/lib/node_modules"
    cp -a "$BREW/lib/node_modules/npm" "$ROOTFS_TMP/usr/lib/node_modules/npm"
    printf '#!/usr/bin/node\nrequire("/usr/lib/node_modules/npm/bin/npm-cli.js")' > "$ROOTFS_TMP/usr/bin/npm"
    chmod +x "$ROOTFS_TMP/usr/bin/npm"
fi

# Claude Code
[ -d "$BREW/lib/node_modules/@anthropic-ai/claude-code" ] && \
    cp -a "$BREW/lib/node_modules/@anthropic-ai/claude-code" "$ROOTFS_TMP/opt/claude-code"

# /etc
printf "nameserver 10.0.2.3" > "$ROOTFS_TMP/etc/resolv.conf"

# Resolve hostnames at build time
resolve() { dig +short "$1" A 2>/dev/null | head -1; }
HOSTS="127.0.0.1 localhost"
for h in registry.npmjs.org example.com api.anthropic.com; do
    ip=$(resolve "$h")
    [ -n "$ip" ] && HOSTS="$HOSTS
$ip $h"
done
printf "%s" "$HOSTS" > "$ROOTFS_TMP/etc/hosts"
printf "hosts: files dns" > "$ROOTFS_TMP/etc/nsswitch.conf"

# Shell profile
cat > "$ROOTFS_TMP/etc/profile" << 'EOPROF'
export PATH=/usr/local/bin:/usr/bin:/bin
export HOME=/home
export TERM=xterm-256color
export LANG=en_US.UTF-8
export NODE_PATH=/usr/lib/node_modules
export SSL_CERT_FILE=/etc/ssl/cert.pem
export PS1="\[\e[1;32m\]cosmo\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ "
cd /home
EOPROF

printf '[ -f /etc/profile ] && . /etc/profile' > "$ROOTFS_TMP/home/.bash_profile"
printf '# CosmoRT user config\nalias ls="ls --color=auto"\nalias ll="ls -la"\nalias grep="grep --color=auto"' > "$ROOTFS_TMP/home/.bashrc"

printf 'root:x:0:0:root:/home:/usr/bin/bash' > "$ROOTFS_TMP/etc/passwd"
printf 'root:x:0:' > "$ROOTFS_TMP/etc/group"

# TLS CA certificate bundle
[ -f "$BREW/ssl/cert.pem" ] && cp "$BREW/ssl/cert.pem" "$ROOTFS_TMP/etc/ssl/cert.pem"
[ -f "$BREW/ssl/cert.pem" ] && cp "$BREW/ssl/cert.pem" "$ROOTFS_TMP/usr/local/ssl/cert.pem"

# Boot-test script
if [ -z "$COSMO_INTERACTIVE" ] && [ -f tools/boot-test.sh ]; then
    cp tools/boot-test.sh "$ROOTFS_TMP/home/.boot"
fi

# ── Step 2: Create ext2 image from rootfs ─────────────
dd if=/dev/zero of="$EXT2_TMP" bs=1M count="$FS_MB" 2>/dev/null
mkfs.ext2 -q -d "$ROOTFS_TMP" "$EXT2_TMP"
rm -rf "$ROOTFS_TMP"

# ── Step 3: Build combined GPT image ───────────────
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
    rm -f "$IMG" "$EXT2_TMP"
    exit 1
fi

# ── Step 4: Format ESP partition (FAT32) ───────────
dd if=/dev/zero of=.esp.tmp bs=512 count="$ESP_SECTORS" 2>/dev/null
mkfs.fat -F 32 .esp.tmp >/dev/null

if [ -f "$EFI_BIN" ]; then
    mmd -i .esp.tmp ::/EFI
    mmd -i .esp.tmp ::/EFI/BOOT
    mcopy -i .esp.tmp "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
else
    echo "WARNING: $EFI_BIN not found, ESP will be empty (run make first)"
fi

dd if=.esp.tmp of="$IMG" bs=512 seek="$ESP_START" conv=notrunc 2>/dev/null
rm -f .esp.tmp

# ── Step 5: Write ext2 into combined image ─────────
dd if="$EXT2_TMP" of="$IMG" bs=512 seek="$FS_START" conv=notrunc 2>/dev/null

echo "disk.img: $(du -h "$IMG" | cut -f1) (GPT: ${ESP_MB}MB ESP + ${FS_MB}MB ext2)"
