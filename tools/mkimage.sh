#!/bin/sh
# Build CosmoRT disk.img with boot-test.sh as .bashrc
# Usage: sh tools/mkimage.sh

set -e
cd "$(dirname "$0")/.."

PX=~/Git/CosmoPX
BREW="$PX/brew/prefix"
IMG=disk.img

# Build host tools
make tools/mkfs tools/cosmo_cp 2>/dev/null || {
    gcc -Wall -Wextra -O2 -o tools/mkfs tools/mkfs.c
    gcc -Wall -Wextra -O2 -o tools/cosmo_cp tools/cosmo_cp.c
}

# Create 512MB CosmoFS image
./tools/mkfs "$IMG" 512

# Directories
for d in usr usr/bin home tmp etc opt opt/claude-code dev; do
    ./tools/cosmo_cp "$IMG" --mkdir "/$d"
done

# Shell + coreutils
for bin in $PX/coreutils/build/*; do
    ./tools/cosmo_cp "$IMG" "$bin" "/usr/bin/$(basename "$bin")"
done
./tools/cosmo_cp "$IMG" "$PX/sh/build/sh" /usr/bin/sh
./tools/cosmo_cp "$IMG" "$BREW/bin/bash" /usr/bin/bash

# Node.js
./tools/cosmo_cp "$IMG" "$BREW/bin/node" /usr/bin/node

# Claude Code
./tools/cosmo_cp "$IMG" --tree "$BREW/lib/node_modules/@anthropic-ai/claude-code" /opt/claude-code

# /etc
./tools/cosmo_cp "$IMG" --write-string "nameserver 10.0.2.3" /etc/resolv.conf
./tools/cosmo_cp "$IMG" --write-string "127.0.0.1 localhost" /etc/hosts

# .bashrc = boot-test.sh
./tools/cosmo_cp "$IMG" tools/boot-test.sh /home/.bashrc

echo "disk.img: $(du -h "$IMG" | cut -f1)"
