# Dynamic Linker — musl ld-musl-x86_64.so.1

CosmoRT nutzt musl libc als System-libc. Der Dynamic Linker ist
`ld-musl-x86_64.so.1` aus dem musl-Paket. Kein eigener Linker noetig.

## Kernel-Interface (CosmoRT liefert)

```
ELF-Loading:
  PT_INTERP erkannt → Interpreter wird statt Binary geladen
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

## Installation

```
/lib/ld-musl-x86_64.so.1    Dynamic Linker (aus musl-Paket)
/lib/libc.so                 musl libc Shared Library
/usr/lib/                    Weitere Shared Libraries
```

## PT_INTERP

Dynamisch gelinkte musl-Binaries haben:
  PT_INTERP = "/lib/ld-musl-x86_64.so.1"

CosmoRT's ELF-Loader erkennt PT_INTERP und laedt den Interpreter.

## Referenzen

- musl libc: https://musl.libc.org/
- musl ldso: https://git.musl-libc.org/cgit/musl/tree/ldso
- ELF Specification: https://refspecs.linuxbase.org/elf/elf.pdf
- System V ABI AMD64: https://gitlab.com/x86-psABIs/x86-64-ABI
