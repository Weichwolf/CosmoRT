# CosmoRT — Offene Punkte

Stand: 2026-03-24. 509 ktest PASS, 14/15 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.

---

## Phase 1 — Remote-Exploits — erledigt

- [x] #5 TCP plen bounds-check
- [x] #1 DNS Response Parsing OOB
- [x] #9 DNS Name Parsing ri2 bounds
- [x] #19 sendto MTU guard

## Phase 2 — ELF Loader — erledigt

- [x] #6 phoff validation
- [x] #2 vaddr+filesz overflow check

## Phase 3 — Stability — erledigt

- [x] #3 AVL-Tree Remove UAF
- [x] #7 Buddy Double-Free guard
- [x] #4 Futex PI lock
- [x] #11 Signal Frame RSP underflow
- [x] #12 Epoll Sleeper NULL check
- [x] #15 Shift UB (1ULL)
- [x] #16 pread64 NULL check

## Phase 4 — claude update

- [x] npm --version (10.9.2)
- [x] getcwd Pointer→Laenge
- [x] INODE_COUNT 8192
- [x] HTTPS registry.npmjs.org
- [x] SIGCHLD-Delivery (bash ueberlebt npm-Exit)
- [ ] npm crasht bei Exit (SIGSEGV in V8 GC — Output korrekt)
- [ ] npm install -g
- [ ] claude update erfolgreich

## Phase 5 — Races/Hardening — erledigt

- [x] #8 sched migration deadlock
- [x] #10 mmap VMA race
- [x] #13 symlink iterativ
- [x] #17 clone3 validation
- [x] #18 prctl TOCTOU
- [x] #20 file offset overflow
- [x] #21 mmap file_off overflow
- [x] #22 PTY line buffer bounds
- [x] #14 E1000 BAR validation

## Phase 6 — Low-Prio — erledigt

- [x] #23 count_rt_threads docs
- [x] #24 copy_path_from_user overflow
- [x] #25 do_write overflow
- [x] #26 readv iovec copy
- [x] #27 utimensat sizeof

## Phase 7 — Einschraenkungen

- [x] Ctrl-C (SIGINT via PTY — war bereits implementiert)
- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] Job Control (bash ohne +m Flag)
- [ ] Dynamic Linker (cat/coreutils crashen, ld-cosmo.so)
- [ ] GPT-Image Boot
