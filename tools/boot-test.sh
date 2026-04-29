#!/bin/sh
# LTP shell tests source tst_test.sh via POSIX `.` (PATH-resolved) und
# rufen tst_sleep/tst_timeout_kill als commands. `make install` kopiert
# weder lib/*.sh noch die kompilierten lib/tst_*-Helper ins install-Tree.
# Source-Tree-Pfad PLUS install-bin (wo wir die Helper hin kompilieren).
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/opt/ltp/testcases/lib:/opt/ltp/install/testcases/bin
export LTPROOT=/opt/ltp/install
# LTP: Kein /proc/config.gz verfuegbar; alle needs_kconfigs wuerden TBROK werfen.
export KCONFIG_SKIP_CHECK=1
# LTP tst_is_virt() ohne systemd-detect-virt fallt auf /proc/cpuinfo "QEMU Virtual CPU"
# zurueck — wir reporten "CosmoRT vCPU". Override teilt LTP mit dass wir in einer VM
# laufen, sodass timing-Tests den 10x delta-Multiplier anwenden.
export LTP_VIRT_OVERRIDE=qemu
# Scheduler/hrtimer 1000Hz periodic-tick: 1ms-sleep wird in der Praxis zu ~2-3ms.
# clock_nanosleep02 + andere tst_timer_test-basierte Tests laufen daher ~3x linger
# als ihre interne tst_set_runtime erwartet. Multiplier verlaengert das Fenster.
export LTP_TIMEOUT_MUL=5
mount -t proc none /proc 2>/dev/null || true
mount -t sysfs none /sys 2>/dev/null || true
mount -t tmpfs none /tmp 2>/dev/null || true
mkdir -p /tmp 2>/dev/null || true
echo "========================================"
echo "  CosmoRT Boot Test"
echo "  $(uname -a)"
echo "========================================"

# Optionale RUN-Listen fuer gezielte Debug-Sessions: glob-Pattern in /opt/{musl,ltp}_run.
# Inhalt nicht leer -> Filter (sh-glob) reduziert die Liste + voller Debug-Output.
# Datei fehlt oder leer -> alle Tests, kein Debug-Output.
MUSL_RUN=""
LTP_RUN=""
[ -f /opt/musl_run ] && MUSL_RUN=$(cat /opt/musl_run)
[ -f /opt/ltp_run  ] && LTP_RUN=$(cat /opt/ltp_run)
DEBUG=0
[ -n "$MUSL_RUN" ] && DEBUG=1
[ -n "$LTP_RUN" ]  && DEBUG=1

dump() {
    echo "--- OUTPUT ($1) ---"
    cat "$2"
    echo "--- END ($1) ---"
}

musl_pass=0; musl_fail=0; musl_skip=0
if [ "$COSMO_SKIP_MUSL" != "1" ] && [ ! -f /opt/skip-musl ]; then
echo ""
echo "=== MUSL LIBC-TEST ==="
cd /opt/libc-test
RUNNER=src/common/runtest.exe
MUSL_EXES=$(find src -name '*.exe' ! -name 'runtest.exe' ! -name 'libtest.a' | sort)
if [ -n "$MUSL_RUN" ]; then
    filtered=""
    for exe in $MUSL_EXES; do
        name=${exe##*/}; name=${name%.exe}
        eval "case \"\$name\" in $MUSL_RUN) filtered=\"\$filtered \$exe\" ;; esac"
    done
    MUSL_EXES=$filtered
fi
musl_total_exes=$(echo $MUSL_EXES | wc -w)
musl_idx=0
for exe in $MUSL_EXES; do
    musl_idx=$((musl_idx + 1))
    name=${exe##*/}; name=${name%.exe}
    echo "[$musl_idx/$musl_total_exes] $name RUN"
    timeout -k 5 60 "$RUNNER" -t 45 -w '' "$exe" > /tmp/musl_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "[$musl_idx/$musl_total_exes] $name PASS"
        [ $DEBUG -eq 1 ] && dump "$name" /tmp/musl_out.txt
        musl_pass=$((musl_pass + 1))
    else
        echo "[$musl_idx/$musl_total_exes] $name FAIL rc=$rc"
        if [ $DEBUG -eq 1 ]; then
            dump "$name" /tmp/musl_out.txt
        else
            echo "--- OUTPUT ($name) ---"
            head -30 /tmp/musl_out.txt
            echo "--- END ($name) ---"
        fi
        musl_fail=$((musl_fail + 1))
    fi
done
echo "musl libc-test: $musl_pass PASS, $musl_fail FAIL, $musl_skip SKIP"
fi

echo ""
echo "=== LTP TESTS ==="
LTP_BIN=/opt/ltp/install/testcases/bin

# LTP shell helpers: tst_sleep + tst_timeout_kill werden von tst_test.sh
# gerufen, sind aber self-contained C-Quellen die `make install` nicht
# ins install-Tree kopiert. Beim Boot kompilieren wenn fehlend.
LTP_LIB=/opt/ltp/testcases/lib
for helper in tst_sleep tst_timeout_kill; do
    if [ ! -x "$LTP_BIN/$helper" ] && [ -f "$LTP_LIB/$helper.c" ]; then
        echo "boot-test: compiling LTP helper $helper..."
        gcc -O2 -o "$LTP_BIN/$helper" "$LTP_LIB/$helper.c" 2>&1 | head -5
    fi
done


ltp_passed=0; ltp_failed=0; ltp_skipped=0; ltp_total=0
LTP_TESTS=$(ls "$LTP_BIN" | grep -v '_helper$' | sort)
if [ -n "$LTP_RUN" ]; then
    filtered=""
    for t in $LTP_TESTS; do
        eval "case \"\$t\" in $LTP_RUN) filtered=\"\$filtered \$t\" ;; esac"
    done
    LTP_TESTS=$filtered
fi
ltp_total_count=$(echo $LTP_TESTS | wc -w)
for t in $LTP_TESTS; do
    ltp_total=$((ltp_total + 1))
    if [ ! -x "$LTP_BIN/$t" ]; then
        ltp_skipped=$((ltp_skipped + 1))
        continue
    fi
    echo "[$ltp_total/$ltp_total_count] $t RUN"
    cd /tmp
    # Timing-basierte Tests brauchen mehr als Default. clock_nanosleep02 macht 1463
    # sleeps mit Iteration-Stichproben (~9s LTP-Runtime + Slack). LTP-timer-Tests
    # folgen demselben Muster.
    case "$t" in
        clock_nanosleep02) tlim=180 ;;
        clock_gettime04)   tlim=60  ;;
        fcntl14|fcntl14_64) tlim=240 ;;
        fcntl34|fcntl34_64|fcntl36|fcntl36_64) tlim=240 ;;
        epoll-ltp)         tlim=120 ;;
        epoll_wait02)      tlim=120 ;;
        connect02)         tlim=180 ;;
        *)                 tlim=10  ;;
    esac
    timeout "$tlim" "$LTP_BIN/$t" > /tmp/ltp_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "[$ltp_total/$ltp_total_count] $t PASS"
        [ $DEBUG -eq 1 ] && dump "$t" /tmp/ltp_out.txt
        ltp_passed=$((ltp_passed + 1))
    elif [ $rc -eq 32 ] || [ $rc -eq 36 ]; then
        echo "[$ltp_total/$ltp_total_count] $t SKIP"
        [ $DEBUG -eq 1 ] && dump "$t" /tmp/ltp_out.txt
        ltp_skipped=$((ltp_skipped + 1))
    else
        echo "[$ltp_total/$ltp_total_count] $t FAIL rc=$rc"
        if [ $DEBUG -eq 1 ]; then
            dump "$t" /tmp/ltp_out.txt
        else
            echo "--- OUTPUT ($t) ---"
            head -80 /tmp/ltp_out.txt
            echo "--- END ($t) ---"
        fi
        ltp_failed=$((ltp_failed + 1))
    fi
done

echo ""
echo "========================================"
echo "  ALL DONE"
echo "  musl: $musl_pass PASS, $musl_fail FAIL"
echo "  LTP:  $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"
echo "========================================"
poweroff -f
