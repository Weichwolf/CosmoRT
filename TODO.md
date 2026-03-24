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

## Audit — 27 Bugs

### CRITICAL (4)

| # | Datei | Bug |
|---|-------|-----|
| 1 | net.c:794 | DNS Response Parsing — Buffer OOB (`ri2` ohne bounds-check) |
| 2 | elf.c:58 | ELF Loader Integer Overflow (`vaddr + p_filesz` wraps) |
| 3 | vma.c:154 | AVL-Tree Remove Use-After-Free (Successor freed nach Copy) |
| 4 | futex.c:205 | Futex PI Use-After-Free (`thread_find_by_tid` ohne Lock) |

### HIGH (5)

| # | Datei | Bug |
|---|-------|-----|
| 5 | net.c:635 | TCP plen negativ → 4GB mcpy |
| 6 | process.c:304 | ELF phoff nicht gegen elf_len validiert |
| 7 | page_alloc.c:89 | Buddy Double-Free (kein Check ob bereits frei) |
| 8 | sched.c:239 | RT Migration Deadlock (Lock dropped, Queue-State ungueltig) |
| 9 | net.c:797 | DNS Name Parsing — ri2 += 4 ohne Bounds-Check |

### MEDIUM (13)

| # | Datei | Bug |
|---|-------|-----|
| 10 | sys_mem.c:387 | mmap VMA Race (Lock dropped fuer I/O) |
| 11 | sys_signal.c:134 | Signal Frame RSP Underflow (stack_rsp - frame_size wraps) |
| 12 | epoll.c:283 | Epoll Sleeper NULL-Deref (Race mit anderem CPU) |
| 13 | vfs.c:234 | Symlink Recursion Stack Exhaustion (8 Levels × 512B) |
| 14 | e1000.c:160 | E1000 BAR nicht validiert (MMIO auf Kernel-Code) |
| 15 | page_alloc.c:131 | order_for_pages() Shift UB (1U << 31+) |
| 16 | sys_file.c:961 | pread64 fde->obj NULL-Deref |
| 17 | sys_proc.c:256 | clone3 uargs nicht pre-validiert |
| 18 | sys_proc.c:316 | prctl PR_SET_NAME TOCTOU |
| 19 | socket.c:187 | sendto: kein MTU-Check (IP+UDP+Payload > 1500) |
| 20 | vfs.c:664 | File Offset uint64 Overflow |
| 21 | sys_mem.c:397 | mmap file_off += 4096 Overflow |
| 22 | pty.c:163 | PTY Line Buffer Bounds vs PTY_LINE_MAX |

### LOW (5)

| # | Datei | Bug |
|---|-------|-----|
| 23 | sched.c:46 | count_rt_threads() ohne Lock |
| 24 | dispatch.c:8 | copy_path_from_user Pointer Overflow |
| 25 | sys_file.c:69 | do_write buf+pos Pointer Overflow |
| 26 | sys_file.c:293 | readv TOCTOU |
| 27 | sys_file.c:570 | utimensat hardcoded 32 Bytes |

### Fix-Prioritaet

1. **Remote:** DNS Parsing (#1, #9), TCP plen (#5) — ein Paket reicht
2. **Local:** ELF Loader (#2, #6) — crafted Binary → Priv-Esc
3. **Stability:** AVL Use-After-Free (#3), Buddy Double-Free (#7)

## Bekannte Einschraenkungen

- c-ares UDP DNS: Kernel liefert korrekte Pakete, c-ares ETIMEOUT
- Ctrl-C, Job Control, Dynamic Linker (cat/coreutils)
- GPT-Image Boot
