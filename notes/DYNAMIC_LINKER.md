# ld-cosmo.so — Dynamic Linker Spezifikation

Anweisung fuer CosmoPX-Entwickler. CosmoRT-Seite ist fertig.

## Kernel-Interface (CosmoRT liefert)

```
ELF-Loading:
  PT_INTERP erkannt → /lib/ld-cosmo.so wird statt Binary geladen
  Auxiliary Vector auf User-Stack:
    AT_PHDR      Adresse der Program Headers des Main-Binary
    AT_PHENT     Groesse eines Program Headers
    AT_PHNUM     Anzahl Program Headers
    AT_ENTRY     Entry-Point des Main-Binary
    AT_BASE      Load-Adresse des Linkers selbst
    AT_PAGESZ    4096
    AT_RANDOM    16 Bytes Zufall (Stack-Canary)
    AT_EXECFN    Pfad des Main-Binary
    AT_NULL      Ende

Syscalls die der Linker braucht:
  mmap(MAP_FIXED)     Segmente laden
  mprotect            RX/RW nach Relocation
  open/read/close     Shared Libraries finden
  write               Fehlerausgabe
  exit_group          Fatal Error
  getrandom           Optional (Stack Canary)
```

## ld-cosmo.so Anforderungen

### ELF Loading

- [ ] Eigene Relocations aufloesen (Bootstrap: Linker muss sich selbst relocaten)
- [ ] Main-Binary PT_LOAD Segmente mappen (mmap MAP_FIXED, Alignment beachten)
- [ ] ET_DYN (PIE): ASLR-Base vom Kernel akzeptieren (AT_PHDR zeigt auf gemappte Adresse)
- [ ] ET_EXEC: Feste Adressen (MAP_FIXED an vaddr)

### .dynamic Section Parsing

- [ ] DT_NEEDED: Liste der benoetigten Shared Libraries
- [ ] DT_SONAME: Canonical Name der Library
- [ ] DT_RPATH / DT_RUNPATH: Library-Suchpfade
- [ ] DT_STRTAB, DT_SYMTAB, DT_STRSZ: String/Symbol-Tabellen
- [ ] DT_HASH / DT_GNU_HASH: Symbol-Hash-Tabelle (GNU_HASH bevorzugen)
- [ ] DT_REL, DT_RELA, DT_JMPREL: Relocation-Tabellen
- [ ] DT_INIT, DT_FINI: Init/Fini Funktionen
- [ ] DT_INIT_ARRAY, DT_FINI_ARRAY: Konstruktoren/Destruktoren
- [ ] DT_FLAGS, DT_FLAGS_1: BIND_NOW, PIE, etc.
- [ ] DT_VERNEED, DT_VERDEF: Symbol-Versioning

### Library Search

Suchpfade in Reihenfolge:
1. DT_RPATH (deprecated, aber gcc nutzt es)
2. LD_LIBRARY_PATH Environment-Variable
3. DT_RUNPATH
4. /lib
5. /usr/lib
6. /usr/local/lib

- [ ] Rekursives Laden: libA.so braucht libB.so braucht libC.so
- [ ] Zirkulaere Dependencies erkennen (Visited-Set)
- [ ] Already-Loaded Check (soname → skip)

### Symbol Resolution

- [ ] GNU Hash Table Lookup (DT_GNU_HASH): Bloom Filter + Bucket + Chain
- [ ] Fallback: SysV Hash (DT_HASH)
- [ ] Global Symbol Table: Breadth-First ueber alle geladenen Objects
- [ ] Weak vs Strong Symbols: Strong gewinnt, Weak nur wenn kein Strong
- [ ] Symbol Versioning (DT_VERNEED): @GLIBC_2.17 etc. (CosmoPX hat eigene Versionen)
- [ ] STB_GNU_UNIQUE: Singleton-Symbole (C++ static)
- [ ] Interposition: LD_PRELOAD Symbole haben Vorrang

### Relocations

x86_64 Relocation Types:
- [ ] R_X86_64_64: Absolute 64-Bit Adresse
- [ ] R_X86_64_GLOB_DAT: GOT Entry fuer globale Variable
- [ ] R_X86_64_JUMP_SLOT: PLT/GOT Entry fuer Funktionsaufruf
- [ ] R_X86_64_RELATIVE: Base-Relative (PIE/Shared Library)
- [ ] R_X86_64_COPY: Copy Relocation (Variable aus .so in Main-Binary)
- [ ] R_X86_64_TPOFF64: Thread-Local Storage Offset
- [ ] R_X86_64_DTPMOD64: TLS Module ID
- [ ] R_X86_64_DTPOFF64: TLS Offset innerhalb Modul
- [ ] R_X86_64_IRELATIVE: Indirect Function (ifunc, CPU-Feature-Dispatch)

### PLT/GOT und Lazy Binding

- [ ] Lazy Binding: PLT springt beim ersten Aufruf in den Linker
- [ ] _dl_runtime_resolve: Symbol aufloesen, GOT patchen, Funktion aufrufen
- [ ] LD_BIND_NOW / DT_FLAGS BIND_NOW: Alle Relocations beim Load aufloesen
- [ ] Audit: seccomp-kompatibel (kein W+X gleichzeitig — resolve in RW, dann mprotect RX)

### Thread-Local Storage (TLS)

- [ ] TLS Initialisation Image (PT_TLS Segment)
- [ ] Static TLS Model (Initial-Exec, Local-Exec): Offset in TCB
- [ ] Dynamic TLS Model (General-Dynamic, Local-Dynamic): __tls_get_addr
- [ ] DTV (Dynamic Thread Vector): pro Thread, pro Modul
- [ ] TLS bei dlopen: neues Modul → DTV erweitern
- [ ] FS_BASE (x86_64): arch_prctl(ARCH_SET_FS) fuer TCB Pointer

### dlopen / dlsym / dlclose

- [ ] dlopen(path, flags): Library zur Laufzeit laden
- [ ] RTLD_LAZY: Lazy Binding
- [ ] RTLD_NOW: Immediate Binding
- [ ] RTLD_GLOBAL: Symbole global sichtbar
- [ ] RTLD_LOCAL: Symbole nur fuer dlsym sichtbar
- [ ] RTLD_NODELETE: Library bleibt nach dlclose geladen
- [ ] dlsym(handle, name): Symbol suchen
- [ ] RTLD_DEFAULT: Globale Suche
- [ ] RTLD_NEXT: Naechstes Object nach Caller (Interposition)
- [ ] dlclose(handle): Refcount--, bei 0: Fini + Unmap
- [ ] dladdr(addr, info): Adresse → Symbol-Info (Debugging)
- [ ] dlerror(): Letzter Fehler als String

### Konstruktoren / Destruktoren

- [ ] DT_INIT: Einmal pro Library aufrufen (nach Relocation, vor main)
- [ ] DT_INIT_ARRAY: Array von Funktionspointern, alle aufrufen
- [ ] DT_FINI: Bei dlclose oder exit
- [ ] DT_FINI_ARRAY: Array, in umgekehrter Reihenfolge
- [ ] Reihenfolge: Dependencies zuerst (topologische Sortierung)
- [ ] __cxa_atexit Integration fuer C++ Destruktoren

### LD_PRELOAD

- [ ] LD_PRELOAD Environment-Variable parsen (Doppelpunkt-separiert)
- [ ] Preload-Libraries vor allen anderen laden
- [ ] Symbol-Interposition: Preload-Symbole haben Vorrang

### Debugging / Audit

- [ ] struct r_debug: Linker-Map fuer gdb (DT_DEBUG)
- [ ] _r_debug.r_brk: Breakpoint-Adresse fuer gdb (Library geladen/entladen)
- [ ] LD_DEBUG Environment-Variable: Trace-Output (libs, reloc, symbols, all)
- [ ] /proc/pid/maps: Geladene Libraries sichtbar (CosmoRT liefert das)

### Error Handling

- [ ] Missing Library: "error: cannot find -lfoo" → stderr + exit(127)
- [ ] Missing Symbol: "undefined symbol: bar" → stderr + exit(127)
- [ ] Version Mismatch: "version GLIBC_2.34 not found" → stderr + exit(127)
- [ ] Circular Dependency: Erkennen und aufloesen (nicht deadlocken)

## Dateien

```
CosmoPX:
  src/ldso/
    ldso.c           Entry-Point, Bootstrap, Main-Loop
    elf.c            ELF Parsing (PT_LOAD, .dynamic, Segments)
    search.c         Library Search (RPATH, LD_LIBRARY_PATH, /lib)
    resolve.c        Symbol Resolution (GNU Hash, Global Table)
    relocate.c       Relocation Engine (alle x86_64 Types)
    tls.c            Thread-Local Storage (Static + Dynamic Model)
    dlopen.c         dlopen/dlsym/dlclose API
    debug.c          r_debug, LD_DEBUG

  build/ld-cosmo.so  → installiert nach /lib/ld-cosmo.so

CosmoRT:
  /lib/ld-cosmo.so   Vom Kernel geladen bei PT_INTERP
  /lib/libc.so        CosmoPX libc als Shared Library
  /usr/lib/           Weitere Shared Libraries
```

## Test-Strategie

```
1. Static Hello World          → laeuft bereits
2. Dynamic Hello World         → ld-cosmo.so + libc.so minimal
3. Dynamic mit dlopen          → dlopen/dlsym/dlclose
4. Dynamic mit TLS             → __thread Variable
5. Dynamic mit C++             → Konstruktoren/Destruktoren, Exceptions
6. Brew-installiertes git      → komplexe Dependency-Chain
7. Node.js dynamisch           → V8 + libuv + OpenSSL als .so
```

## Performance-Ziele

- Library-Load: <1ms pro .so (mmap + relocate)
- Symbol-Lookup: O(1) via GNU Hash
- Lazy Binding Overhead: ~5ns pro erster Aufruf (GOT Patch)
- Startup (Node.js): <50ms fuer alle Libraries

## Referenzen

- ELF Specification (TIS): https://refspecs.linuxbase.org/elf/elf.pdf
- System V ABI AMD64: https://gitlab.com/x86-psABIs/x86-64-ABI
- glibc ld.so: https://sourceware.org/glibc/wiki/Ld.so
- musl ldso/: https://git.musl-libc.org/cgit/musl/tree/ldso
- Linux ELF Loader: fs/binfmt_elf.c
