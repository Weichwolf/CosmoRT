# CosmoRT — Offene Punkte

Stand: 2026-03-25. 509 ktest PASS, 14/15 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.
Audit: 27/27 Security-Fixes erledigt.

---

## claude update

- [ ] npm crasht bei Exit (SIGSEGV in V8 GC — Output korrekt)
- [ ] npm install -g
- [ ] claude update erfolgreich

## Offen

- [ ] c-ares UDP DNS (Kernel liefert korrekte Pakete, c-ares ETIMEOUT)
- [ ] Job Control (TIOCSPGRP, SIGTSTP/SIGCONT)
- [ ] Dynamic Linker (ld-cosmo.so — CosmoPX)
- [ ] GPT-Image Boot (Partitions-Support)
