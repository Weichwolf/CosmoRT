#!/bin/sh
echo "========================================"
echo "  CosmoRT Boot Test"
echo "  $(uname -a)"
echo "========================================"

echo ""
echo "=== MUSL LIBC-TEST ==="
cd /opt/libc-test
rm -f src/*/*.err
RUNNER=src/common/runtest.exe
# musl 1.2.5 bugs (fail with ld-musl on host too — not kernel issues)
# mntent: getmntent 4-field parsing (fixed after musl 1.2.5, commit b4b1e10)
# strptime: %F/%s/%z parsing broken in musl 1.2.5
SKIP="mntent mntent-static strptime strptime-static"
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
    fi
done
echo "musl libc-test: $musl_pass PASS, $musl_fail FAIL"

echo ""
echo "=== LTP REQUIRED TESTS ==="
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
    timeout 10 "$LTP_BIN/$t" > /tmp/ltp_out.txt 2>&1
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
        if [ $ltp_failed -le 5 ]; then
            cat /tmp/ltp_out.txt
        fi
    fi
done < /opt/ltp_required.txt

echo ""
echo "========================================"
echo "  ALL DONE"
echo "  musl: $musl_pass PASS, $musl_fail FAIL"
echo "  LTP:  $ltp_passed PASS, $ltp_failed FAIL, $ltp_skipped SKIP"
echo "========================================"
poweroff -f
