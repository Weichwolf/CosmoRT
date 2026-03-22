#!/usr/bin/bash
# CosmoOS Boot Test — runs as /home/cosmo/.bashrc
# Serial output goes to host. Agent reads it, fixes kernel until PASS.

echo "=== CosmoOS Boot Test ==="
echo "DATE: $(date 2>/dev/null || echo unknown)"

# T1: Basic shell
echo "--- T1: Shell ---"
echo "T1.1 echo: $(echo ok)"
echo "T1.2 arithmetic: $((6 * 7))"
echo "T1.3 uname: $(uname -a 2>/dev/null || echo ENOSYS)"
echo "T1.4 pwd: $(pwd)"
echo "T1.5 ls: $(ls /usr/bin 2>/dev/null || echo FAIL)"

# T2: Node.js
echo "--- T2: Node.js ---"
if [ -x /usr/bin/node ]; then
    echo "T2.1 node exists: ok"
    echo "T2.2 node version: $(/usr/bin/node --version 2>&1)"
    echo "T2.3 node hello: $(/usr/bin/node -e "console.log('hello from CosmoRT')" 2>&1)"
    echo "T2.4 node fs: $(/usr/bin/node -e "require('fs').writeFileSync('/tmp/t','ok');console.log(require('fs').readFileSync('/tmp/t','utf8'))" 2>&1)"
    echo "T2.5 node crypto: $(/usr/bin/node -e "console.log(require('crypto').randomBytes(4).toString('hex'))" 2>&1)"
else
    echo "T2: SKIP (no /usr/bin/node)"
fi

# T3: Claude Code
echo "--- T3: Claude Code ---"
if [ -f /opt/claude-code/cli.js ]; then
    echo "T3.1 cli.js exists: ok"
    echo "T3.2 claude version: $(/usr/bin/node /opt/claude-code/cli.js --version 2>&1)"
else
    echo "T3: SKIP (no /opt/claude-code/cli.js)"
fi

# T4: Network
echo "--- T4: Network ---"
echo "T4.1 dns: $(/usr/bin/node -e "require('dns').resolve('example.com',(e,a)=>{console.log(e||a[0]);process.exit()})" 2>&1)"
echo "T4.2 https: $(/usr/bin/node -e "require('https').get('https://api.anthropic.com',(r)=>{console.log(r.statusCode);process.exit()})" 2>&1)"

echo "=== Boot Test Done ==="
