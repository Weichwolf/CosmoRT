#!/bin/sh
# Single-shot gcc-compile benchmark for CosmoRT.
# Used as /etc/inittab sysinit during qemu-bench. Compiles tst_sleep.c with
# different gcc phases, prints elapsed wall-time per phase, then powers off.

mount -t proc none /proc 2>/dev/null || true
mount -t sysfs none /sys 2>/dev/null || true
mount -t tmpfs none /tmp 2>/dev/null || true

export PATH=/bin:/sbin:/usr/bin:/usr/sbin

SRC=/opt/ltp/testcases/lib/tst_sleep.c
[ ! -f "$SRC" ] && { echo "BENCH: $SRC missing"; poweroff -f; }

echo ""
echo "================================"
echo "  COSMO-BENCH gcc tst_sleep.c"
echo "================================"

bench() {
    label="$1"; shift
    # /proc/uptime: "<seconds>.<centiseconds> <idle>" — works under busybox.
    t0=$(awk '{printf "%d", $1*100}' /proc/uptime)
    "$@" >/dev/null 2>&1
    rc=$?
    t1=$(awk '{printf "%d", $1*100}' /proc/uptime)
    elapsed_cs=$(( t1 - t0 ))
    elapsed_ms=$(( elapsed_cs * 10 ))
    echo "BENCH $label rc=$rc elapsed_ms=$elapsed_ms"
}

# Skip exec-storm microbenches — they're irrelevant for ext4 throughput.
# Focus: gcc-compile end-to-end (headers + cc1 + as + ld + libc.so) and
# a pure ext4 read of a known file (libc.so.1 reads its own bytes).
bench "preprocess-cold" gcc -E -O2 "$SRC" -o /tmp/x.i
bench "preprocess-warm" gcc -E -O2 "$SRC" -o /tmp/x.i
bench "compile-c-warm"  gcc -c -O2 "$SRC" -o /tmp/x.o
bench "full-link-warm"  gcc    -O2 "$SRC" -o /tmp/x
bench "cat-libc-cold"   sh -c 'cat /lib/ld-musl-x86_64.so.1 > /dev/null'
bench "cat-libc-warm"   sh -c 'cat /lib/ld-musl-x86_64.so.1 > /dev/null'
# Pure cc1 invocation (skip gcc driver fork+exec chain)
CC1=$(gcc -print-prog-name=cc1 2>/dev/null)
[ -x "$CC1" ] && bench "cc1-direct" "$CC1" -E "$SRC" -o /tmp/x.i
# How fast is just exec'ing /usr/bin/gcc and immediately exiting?
bench "gcc-version"     gcc --version
# Rapid tmpfs bench (no disk)
bench "tmpfs-write-1M"  sh -c 'dd if=/dev/zero of=/tmp/x bs=4k count=256 2>/dev/null'
# Stat-only: 100x stat /lib/* — pure inode lookup speed
bench "stat-libc-100"   sh -c 'i=0; while [ $i -lt 100 ]; do stat /lib/ld-musl-x86_64.so.1 >/dev/null; i=$((i+1)); done'

echo "================================"
echo "  COSMO-BENCH DONE"
echo "================================"

# Diagnostic: TLB flush fast vs. slow path counters (per-syscall conditional shootdown).
echo "--- TLB flush stats ---"
cat /proc/cosmort/tlb_flush 2>/dev/null || echo "no /proc/cosmort/tlb_flush"
echo "--- Page cache stats ---"
cat /proc/cosmort/page_cache 2>/dev/null || echo "no /proc/cosmort/page_cache"

sync
poweroff -f
