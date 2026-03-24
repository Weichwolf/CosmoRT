#!/usr/bin/bash
# CosmoRT Boot Test — runs as /home/.bashrc
# Tests: T1 Shell, T2 Node.js, T3 Claude Code, T4 Network

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }

check() {
    local desc="$1"; shift
    if "$@" 2>&1; then
        ok "$desc"
    else
        fail "$desc"
    fi
}

echo "=== CosmoRT Boot Test ==="

# ── T1: Shell Basics ──
echo "--- T1: Shell ---"

# echo — bash builtin via subshell
X="$(echo hello)"
if [[ "$X" = "hello" ]]; then ok "echo"; else fail "echo ($X)"; fi
# arithmetic — pure bash
if [[ "$((2+3))" = "5" ]]; then ok "arithmetic"; else fail "arithmetic"; fi
# printf -v — no subshell needed
if printf -v X "%d" 42 && [[ "$X" = "42" ]]; then ok "printf"; else fail "printf"; fi
# variable expansion
X="world"
if [[ "hello $X" = "hello world" ]]; then ok "variable"; else fail "variable"; fi
# test builtin
if [[ 3 -gt 2 ]]; then ok "test"; else fail "test"; fi

# ── T2: Node.js ──
echo "--- T2: Node.js ---"
check "node --version" node --version
X="$(node -e 'console.log("hi")')"
if [[ "$X" = "hi" ]]; then ok "node hello"; else fail "node hello ($X)"; fi
check "node fs" node -e "require('fs').writeFileSync('/tmp/t','ok'); process.exit(require('fs').readFileSync('/tmp/t','utf8')==='ok'?0:1)"
check "node crypto" node -e "require('crypto').randomBytes(16); console.log('ok')"

# ── T3: Claude Code ──
echo "--- T3: Claude Code ---"
check "claude --version" node /opt/claude-code/cli.js --version

# ── T4: Network ──
echo "--- T4: Network ---"
# dns.lookup uses getaddrinfo → /etc/hosts first (nsswitch: files dns)
X="$(node -e 'require("dns").lookup("example.com",(e,a)=>{console.log(e?"ERR:"+e.code:"OK:"+a);process.exit(e?1:0)})' 2>&1)"
if [ $? -eq 0 ]; then ok "dns ($X)"; else fail "dns ($X)"; fi
check "https" node -e "require('https').get('https://example.com',r=>{console.log(r.statusCode);process.exit(r.statusCode===200?0:1)})"

echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -eq 0 ]; then
    echo "=== ALL PASS ==="
else
    echo "=== SOME FAILED ==="
fi

echo "=== BOOT TEST COMPLETE ==="
exit 0
