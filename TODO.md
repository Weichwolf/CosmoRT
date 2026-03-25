# CosmoRT — Offene Punkte

Stand: 2026-03-25. 509 ktest PASS, 15/15 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.
Audit: 27/27 Security-Fixes erledigt.

---

## claude update

- [x] npm --version (10.9.2)
- [x] HTTPS registry.npmjs.org
- [x] SIGCHLD-Delivery
- [x] brk_ceiling Fast-Reject
- [x] Alle Syscalls aus CosmoPX implementiert
- [ ] npm install -g: Funktional moeglich, QEMU-Perf-limitiert (~60s npm startup)
- [ ] claude update: Abhaengig von npm install

## Offen

- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] Job Control (TIOCSPGRP, SIGTSTP/SIGCONT)
- [ ] Dynamic Linker (ld-cosmo.so — CosmoPX)
- [ ] GPT-Image Boot (Partitions-Support)
