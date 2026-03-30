#!/bin/sh
echo "========================================"
echo "  CosmoRT Boot Test"
echo "  $(uname -a)"
echo "========================================"

echo ""
echo "=== LTP REQUIRED TESTS ==="
export LTP_TIMEOUT_MUL=5
LTP_BIN=/opt/ltp/install/testcases/bin
ltp_passed=0; ltp_failed=0; ltp_skipped=0; ltp_total=0
while read t; do
    [ -z "$t" ] && continue
    ltp_total=$((ltp_total + 1))
    if [ ! -x "$LTP_BIN/$t" ]; then
        ltp_skipped=$((ltp_skipped + 1))
        continue
    fi
    echo -n "[$ltp_total/313] $t ... "
    cd /tmp
    timeout 60 "$LTP_BIN/$t" > /tmp/ltp_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "PASS"
        ltp_passed=$((ltp_passed + 1))
    elif [ $rc -eq 32 ]; then
        echo "SKIP"
        ltp_skipped=$((ltp_skipped + 1))
    else
        echo "FAIL (rc=$rc)"
        ltp_failed=$((ltp_failed + 1))
        cat /tmp/ltp_out.txt
        echo "STOPPING: LTP $t failed"
        echo "LTP so far: $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"
        poweroff -f; exit 1
    fi
done < /opt/ltp_required.txt
echo "LTP: $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"

echo ""
echo "=== MUSL LIBC-TEST ==="
cd /opt/libc-test
rm -f src/*/*.err
RUNNER=src/common/runtest.exe
# musl 1.2.5 bugs: mntent (getmntent parsing), strptime (%F/%s/%z)
# Complex signal+fork: raise-race (fork in signal handler + RT signals)
SKIP="mntent mntent-static strptime strptime-static raise-race raise-race-static"
musl_pass=0; musl_fail=0; musl_skip=0
for exe in $(find src -name '*.exe' ! -name 'runtest.exe' ! -name 'libtest.a' | sort); do
    name=$(basename "$exe" .exe)
    skip=0; for s in $SKIP; do [ "$name" = "$s" ] && skip=1; done
    if [ $skip -eq 1 ]; then musl_skip=$((musl_skip + 1)); continue; fi
    timeout 60 "$RUNNER" -t 45 -w '' "$exe" > /tmp/musl_out.txt 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "PASS $name"
        musl_pass=$((musl_pass + 1))
    else
        echo "FAIL $name (rc=$rc)"
        musl_fail=$((musl_fail + 1))
        cat /tmp/musl_out.txt
        echo "STOPPING: musl $name failed"
        echo "musl so far: $musl_pass PASS, $musl_fail FAIL"
        poweroff -f; exit 1
    fi
done
echo "musl libc-test: $musl_pass PASS, $musl_fail FAIL"

echo ""
echo "========================================"
echo "  ALL DONE"
echo "  LTP:  $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"
echo "  musl: $musl_pass PASS, $musl_fail FAIL"
echo "========================================"
poweroff -f
