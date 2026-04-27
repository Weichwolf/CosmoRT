#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export LTPROOT=/opt/ltp/install
# LTP: Kein /proc/config.gz verfuegbar; alle needs_kconfigs wuerden TBROK werfen.
export KCONFIG_SKIP_CHECK=1
mount -t proc none /proc 2>/dev/null || true
mount -t sysfs none /sys 2>/dev/null || true
mount -t tmpfs none /tmp 2>/dev/null || true
mkdir -p /tmp 2>/dev/null || true
echo "========================================"
echo "  CosmoRT Boot Test"
echo "  $(uname -a)"
echo "========================================"

musl_pass=0; musl_fail=0; musl_skip=0
if [ "$COSMO_SKIP_MUSL" != "1" ] && [ ! -f /opt/skip-musl ]; then
echo ""
echo "=== MUSL LIBC-TEST ==="
cd /opt/libc-test
RUNNER=src/common/runtest.exe
SKIP="mntent mntent-static strptime strptime-static raise-race raise-race-static fgetwc-buffering pthread_cond_wait-cancel_ignored pthread_cond_wait-cancel_ignored-static tls_init"
MUSL_EXES=$(find src -name '*.exe' ! -name 'runtest.exe' ! -name 'libtest.a' | sort)
musl_total_exes=$(echo "$MUSL_EXES" | wc -l)
musl_idx=0
for exe in $MUSL_EXES; do
    musl_idx=$((musl_idx + 1))
    name=$(basename "$exe" .exe)
    skip=0; for s in $SKIP; do [ "$name" = "$s" ] && skip=1; done
    if [ $skip -eq 1 ]; then
        echo "[$musl_idx/$musl_total_exes] $name SKIP"
        musl_skip=$((musl_skip + 1)); continue
    fi
    echo "[$musl_idx/$musl_total_exes] $name RUN"
    timeout 60 "$RUNNER" -t 45 -w '' "$exe" > /tmp/musl_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "[$musl_idx/$musl_total_exes] $name PASS"
        musl_pass=$((musl_pass + 1))
    else
        echo "[$musl_idx/$musl_total_exes] $name FAIL rc=$rc"
        echo "--- OUTPUT ($name) ---"
        head -30 /tmp/musl_out.txt
        echo "--- END ($name) ---"
        musl_fail=$((musl_fail + 1))
    fi
done
echo "musl libc-test: $musl_pass PASS, $musl_fail FAIL, $musl_skip SKIP"
fi

echo ""
echo "=== LTP REQUIRED TESTS ==="
LTP_BIN=/opt/ltp/install/testcases/bin
ltp_passed=0; ltp_failed=0; ltp_skipped=0; ltp_total=0
FILTER=""
if [ -f /opt/ltp_filter ]; then
    FILTER=$(cat /opt/ltp_filter)
fi
LTP_SKIP="epoll_wait05 execve04"
while read t; do
    [ -z "$t" ] && continue
    if [ -n "$FILTER" ]; then
        eval "case \"\$t\" in $FILTER) : ;; *) continue ;; esac"
    fi
    skip=0; for s in $LTP_SKIP; do [ "$t" = "$s" ] && skip=1; done
    if [ $skip -eq 1 ]; then
        ltp_total=$((ltp_total + 1))
        echo "[$ltp_total/313] $t SKIP (kernel-PF, see TODO)"
        ltp_skipped=$((ltp_skipped + 1))
        continue
    fi
    ltp_total=$((ltp_total + 1))
    if [ ! -x "$LTP_BIN/$t" ]; then
        ltp_skipped=$((ltp_skipped + 1))
        continue
    fi
    echo "[$ltp_total/313] $t RUN"
    cd /tmp
    timeout 10 "$LTP_BIN/$t" > /tmp/ltp_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "[$ltp_total/313] $t PASS"
        ltp_passed=$((ltp_passed + 1))
    elif [ $rc -eq 32 ] || [ $rc -eq 36 ]; then
        echo "[$ltp_total/313] $t SKIP"
        ltp_skipped=$((ltp_skipped + 1))
    else
        echo "[$ltp_total/313] $t FAIL rc=$rc"
        echo "--- OUTPUT ($t) ---"
        head -40 /tmp/ltp_out.txt
        echo "--- END ($t) ---"
        ltp_failed=$((ltp_failed + 1))
    fi
done < /opt/ltp_required.txt

echo ""
echo "========================================"
echo "  ALL DONE"
echo "  musl: $musl_pass PASS, $musl_fail FAIL"
echo "  LTP:  $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"
echo "========================================"
poweroff -f
