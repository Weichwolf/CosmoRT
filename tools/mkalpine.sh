#!/bin/sh
# Build ext2 image containing Alpine Linux with bash, gcc, Node.js, and test suites.
# Usage: sh tools/mkalpine.sh [ALPINE_ROOT]
#
# First run: downloads Alpine minirootfs, installs packages, builds test suites.
# Subsequent runs: reuses existing ALPINE_ROOT (fast — only mkfs.ext2).
#
# Boot chain: kernel init.c → /sbin/init → getty → login → bash (.bashrc)
# .bashrc runs test suites on first boot.

set -e
cd "$(dirname "$0")/.."

ALPINE_ROOT="${1:-build/alpine-root}"
IMG=build/disk.img
ESP_MB=64
FS_MB=2048
EXT2_TMP=build/alpine.img
EFI_BIN=build/BOOTX64.EFI
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.3-x86_64.tar.gz"

# ── Step 0: Bootstrap Alpine rootfs if missing ───────
if [ ! -f "$ALPINE_ROOT/bin/busybox" ]; then
    echo "mkalpine: downloading Alpine minirootfs..."
    mkdir -p "$ALPINE_ROOT"
    wget -qO- "$ALPINE_URL" | tar xzf - -C "$ALPINE_ROOT"
fi

# ── Step 1: Install packages + build test suites + configure ──
if [ ! -f "$ALPINE_ROOT/.mkalpine-done" ]; then
    echo "mkalpine: installing packages and building test suites..."

    # Bind mount individual dev nodes + /proc (safe — no full /dev mount)
    sudo mkdir -p "$ALPINE_ROOT/dev" "$ALPINE_ROOT/proc" 2>/dev/null || true
    sudo mount --bind /proc "$ALPINE_ROOT/proc" 2>/dev/null || true
    for d in null zero urandom random; do
        sudo touch "$ALPINE_ROOT/dev/$d" 2>/dev/null || true
        sudo mount --bind "/dev/$d" "$ALPINE_ROOT/dev/$d" 2>/dev/null || true
    done

    sudo chroot "$ALPINE_ROOT" /bin/sh -c '
        set -e
        echo "nameserver 8.8.8.8" > /etc/resolv.conf

        # ── Packages ──
        echo ">>> Installing packages..."
        apk add --no-cache \
            bash gcc musl-dev make git nodejs npm \
            stress-ng linux-headers autoconf automake libtool m4 pkgconf


        # ── LTP (Linux Test Project) ──
        if [ ! -d /opt/ltp/testcases ]; then
            echo ">>> Cloning LTP..."
            mkdir -p /opt
            cd /opt
            git clone --depth 1 https://github.com/linux-test-project/ltp.git
            cd ltp
            echo ">>> Building LTP..."
            make autotools
            ./configure --prefix=/opt/ltp/install
            make -j4 -k || true
            make install -k || true
            echo ">>> LTP built"
        fi

        # ── musl libc-test ──
        if [ ! -d /opt/libc-test/src ]; then
            echo ">>> Cloning musl libc-test..."
            cd /opt
            git clone --depth 1 https://repo.or.cz/libc-test.git
            cd libc-test
            echo ">>> Building musl libc-test..."
            make -j4 || true
            echo ">>> musl libc-test built"
        fi

        # ── /root/.bash_profile ──
        cat > /root/.bash_profile << "PROFILE"
[ -f ~/.bashrc ] && . ~/.bashrc
PROFILE

        # ── /root/.bashrc: auto-run tests on first boot ──
        cat > /root/.bashrc << "BASHRC"
if [ ! -f /tmp/.tests-done ]; then
    echo "========================================"
    echo "  CosmoRT Test Suite Runner"
    echo "========================================"
    /bin/bash /tmp/test_suites.sh 2>&1 | tee /tmp/test_results.txt
    touch /tmp/.tests-done
fi
BASHRC

        # ── /tmp/test_suites.sh ──
        cat > /tmp/test_suites.sh << "TESTS"
#!/bin/bash
echo ""
echo "=== MUSL LIBC-TEST ==="
cd /opt/libc-test
make 2>&1 | grep -E "^(PASS|FAIL)" | tee /tmp/musl_results.txt
pass=$(grep -c "^PASS" /tmp/musl_results.txt 2>/dev/null || echo 0)
fail=$(grep -c "^FAIL" /tmp/musl_results.txt 2>/dev/null || echo 0)
echo ""
echo "musl libc-test: $pass PASS, $fail FAIL"
if [ "$fail" -gt 0 ]; then
    echo "Failures:"
    grep "^FAIL" /tmp/musl_results.txt
fi

echo ""
echo "=== LTP SYSCALL TESTS ==="
cd /opt/ltp
failed=0
passed=0
skipped=0
for t in $(find . -path "*/kernel/syscalls/*" -type f -executable 2>/dev/null | sort); do
    name=$(basename "$t")
    timeout 10 "$t" > /tmp/ltp_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        passed=$((passed + 1))
    elif [ $rc -eq 32 ]; then
        skipped=$((skipped + 1))
    else
        failed=$((failed + 1))
        echo "FAIL $name (rc=$rc)"
    fi
done
echo ""
echo "LTP syscalls: $passed PASS, $failed FAIL, $skipped SKIP"

echo ""
echo "========================================"
echo "  SUMMARY"
echo "  musl: $pass PASS, $fail FAIL"
echo "  LTP:  $passed PASS, $failed FAIL, $skipped SKIP"
echo "========================================"
TESTS
        chmod +x /tmp/test_suites.sh

        echo ">>> Configuration done"
    '

    sudo chmod -R a+r "$ALPINE_ROOT/" 2>/dev/null || true
    touch "$ALPINE_ROOT/.mkalpine-done"
    echo "mkalpine: packages and test suites ready"
else
    echo "mkalpine: reusing existing $ALPINE_ROOT"
fi

# Unmount leftover bind mounts
for d in null zero urandom random; do
    sudo umount "$ALPINE_ROOT/dev/$d" 2>/dev/null || true
done
sudo umount "$ALPINE_ROOT/proc" 2>/dev/null || true

# ── Step 2: Create ext2 image ────────────────────────
echo "mkalpine: creating ext2 ($FS_MB MB) from $ALPINE_ROOT"
dd if=/dev/zero of="$EXT2_TMP" bs=1M count="$FS_MB" 2>/dev/null
mkfs.ext2 -q -N 65536 -d "$ALPINE_ROOT" "$EXT2_TMP"

# Inject resolv.conf for QEMU SLIRP
if command -v debugfs >/dev/null 2>&1; then
    echo "nameserver 10.0.2.3" | debugfs -w -R "write /dev/stdin /etc/resolv.conf" "$EXT2_TMP" 2>/dev/null || true
fi

# ── Step 3: Build GPT disk image ────────────────────
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

# ── Step 4: Format ESP (FAT32) ──────────────────────
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

# ── Step 5: Write ext2 into GPT ────────────────────
dd if="$EXT2_TMP" of="$IMG" bs=512 seek="$FS_START" conv=notrunc 2>/dev/null

echo "alpine.img: $(du -h "$EXT2_TMP" | cut -f1) ext2"
echo "disk.img:   $(du -h "$IMG" | cut -f1) (GPT: ${ESP_MB}MB ESP + ${FS_MB}MB ext2)"
