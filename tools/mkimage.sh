#!/bin/sh
# Build CosmoOS disk.img with boot test
set -e

RT=$(cd "$(dirname "$0")/.." && pwd)
PX=~/Git/CosmoPX
CP=$RT/tools/cosmo_cp

# Build mkfs if needed
[ -x $RT/tools/mkfs ] || gcc -o $RT/tools/mkfs $RT/tools/mkfs.c -Wall -Wextra

# Create image
$RT/tools/mkfs $RT/disk.img 512

# Directories
for d in /usr /usr/bin /opt /opt/claude-code /home /home/cosmo /tmp /etc /dev; do
    $CP $RT/disk.img --mkdir $d
done

# Binaries (all static)
$CP $RT/disk.img $PX/brew/prefix/bin/node /usr/bin/node
$CP $RT/disk.img $PX/brew/prefix/bin/bash /usr/bin/bash
$CP $RT/disk.img $PX/brew/prefix/bin/git /usr/bin/git

# Config
$CP $RT/disk.img --write-string "nameserver 10.0.2.3" /etc/resolv.conf
$CP $RT/disk.img --write-string "127.0.0.1 localhost" /etc/hosts
$CP $RT/disk.img --write-string "root:x:0:0::/home/cosmo:/usr/bin/bash" /etc/passwd

# Boot test as .bashrc
$CP $RT/disk.img $RT/tools/boot-test.sh /home/cosmo/.bashrc

# Claude Code
$CP $RT/disk.img --tree $PX/brew/prefix/lib/node_modules/@anthropic-ai/claude-code /opt/claude-code

echo "=== disk.img ready ==="
