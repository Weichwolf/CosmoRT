# CosmoRT — Offene Punkte

Stand: 2026-03-24. 509 ktest PASS, 12/12 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS ok.

---

## claude update

- [x] npm --version funktioniert (10.9.2, ~60s Startup)
- [x] getcwd: Return-Wert war Pointer statt Laenge (musl-Inkompatibilitaet)
- [x] INODE_COUNT 1024→8192 (npm hat 2778 Dateien)
- [ ] bash crasht nach npm-Exit (SEGFAULT pid=1, Memory-Pressure?)
- [ ] npm install -g (HTTPS zu registry.npmjs.org, Dateisystem-Schreibzugriffe)
- [ ] claude update erfolgreich

## Bekannte Einschraenkungen

- c-ares UDP DNS: Kernel liefert korrekte Pakete, c-ares ETIMEOUT
- Ctrl-C (SIGINT an Foreground-Prozessgruppe via PTY)
- Dynamic Linker: cat/coreutils crashen (RIP=0x0, ld-cosmo.so)
- Job Control (bash ohne +m Flag)
- GPT-Image Boot (Kernel hat keinen Partitions-Support)
