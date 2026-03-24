# CosmoRT — Offene Punkte

Stand: 2026-03-24. 509 ktest PASS, 14/14 Boot-Test PASS.
SMP 2. RT+Compute Core-Modell.
Node.js v22.14.0 + Claude Code 2.1.81 + npm 10.9.2 laufen.
DHCP, DNS (lookup), HTTPS (incl. registry.npmjs.org) ok.

---

## Phase 1 — Remote-Exploits (ein Paket reicht)

- [ ] #5 net.c:635 — TCP plen negativ → 4GB mcpy (1-Zeiler Fix)
- [ ] #1 net.c:794 — DNS Response Parsing Buffer OOB
- [ ] #9 net.c:797 — DNS Name Parsing ri2 += 4 ohne Bounds-Check
- [ ] #19 socket.c:187 — sendto kein MTU-Check

## Phase 2 — Local Priv-Esc (crafted ELF)

- [ ] #6 process.c:304 — ELF phoff nicht gegen elf_len validiert
- [ ] #2 elf.c:58 — ELF Loader Integer Overflow (vaddr + p_filesz wraps)

## Phase 3 — Kernel-Stability (Crashes/Corruption)

- [ ] #3 vma.c:154 — AVL-Tree Remove Use-After-Free
- [ ] #7 page_alloc.c:89 — Buddy Double-Free
- [ ] #4 futex.c:205 — Futex PI Use-After-Free
- [ ] #11 sys_signal.c:134 — Signal Frame RSP Underflow
- [ ] #12 epoll.c:283 — Epoll Sleeper NULL-Deref
- [ ] #15 page_alloc.c:131 — order_for_pages() Shift UB
- [ ] #16 sys_file.c:961 — pread64 fde->obj NULL-Deref

## Phase 4 — claude update (Feature-Blocker)

- [x] npm --version funktioniert (10.9.2)
- [x] getcwd: Return-Wert Pointer→Laenge (musl-Compat)
- [x] INODE_COUNT 1024→8192
- [x] HTTPS zu registry.npmjs.org PASS
- [ ] bash crasht nach npm-Exit (SEGFAULT pid=1, SIGCHLD-Handler?)
- [ ] npm install -g
- [ ] claude update erfolgreich

## Phase 5 — Races/Hardening

- [ ] #8 sched.c:239 — RT Migration Deadlock
- [ ] #10 sys_mem.c:387 — mmap VMA Race
- [ ] #13 vfs.c:234 — Symlink Recursion Stack Exhaustion
- [ ] #17 sys_proc.c:256 — clone3 uargs nicht pre-validiert
- [ ] #18 sys_proc.c:316 — prctl PR_SET_NAME TOCTOU
- [ ] #20 vfs.c:664 — File Offset uint64 Overflow
- [ ] #21 sys_mem.c:397 — mmap file_off Overflow
- [ ] #22 pty.c:163 — PTY Line Buffer Bounds
- [ ] #14 e1000.c:160 — E1000 BAR nicht validiert

## Phase 6 — Low-Prio Hardening

- [ ] #23 sched.c:46 — count_rt_threads() ohne Lock
- [ ] #24 dispatch.c:8 — copy_path_from_user Pointer Overflow
- [ ] #25 sys_file.c:69 — do_write buf+pos Pointer Overflow
- [ ] #26 sys_file.c:293 — readv TOCTOU
- [ ] #27 sys_file.c:570 — utimensat hardcoded 32 Bytes

## Phase 7 — Einschraenkungen

- [ ] c-ares UDP DNS (c-ares ETIMEOUT trotz korrekter Pakete)
- [ ] Ctrl-C (SIGINT an Foreground-Prozessgruppe via PTY)
- [ ] Job Control (bash ohne +m Flag)
- [ ] Dynamic Linker (cat/coreutils crashen, ld-cosmo.so)
- [ ] GPT-Image Boot
