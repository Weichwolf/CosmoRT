#!/bin/sh
# Run all LTP mini-tests, tally results
pass=0
fail=0
skip=0

dir=$(dirname "$0")

for t in "$dir"/ltp_*; do
    [ -x "$t" ] || continue
    # skip files with extensions (source, headers)
    case "$t" in *.c|*.h|*.o) continue;; esac
    echo "--- $(basename "$t") ---"
    if "$t"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
    echo
done

total=$((pass + fail))
echo "==============================="
echo "LTP: $pass/$total suites passed"
[ "$fail" -gt 0 ] && echo "     $fail suites FAILED"
echo "==============================="
exit $fail
