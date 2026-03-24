# CosmoRT — Offene Punkte

Stand: 2026-03-24. 509 ktest PASS, 14/14 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.

---

## claude update

- [x] npm --version funktioniert (10.9.2)
- [x] getcwd: Return-Wert Pointer→Laenge (musl-Compat)
- [x] INODE_COUNT 1024→8192
- [x] HTTPS zu registry.npmjs.org PASS
- [ ] bash crasht nach npm-Exit (SEGFAULT pid=1, SIGCHLD-Handler?)
- [ ] npm install -g (braucht funktionierenden bash-Subprocess)
- [ ] claude update erfolgreich

## Audit (27 Bugs, siehe Audit-Report)

Top-3 Prioritaet:
- [ ] DNS Parsing Buffer-OOB (Remote-Exploit)
- [ ] TCP plen negativ → 4GB mcpy (Remote-Crash)
- [ ] ELF Loader Integer-Overflow (Local Priv-Esc)

## Bekannte Einschraenkungen

- c-ares UDP DNS: Kernel liefert korrekte Pakete, c-ares ETIMEOUT
- Ctrl-C, Job Control, Dynamic Linker (cat/coreutils)
- GPT-Image Boot
