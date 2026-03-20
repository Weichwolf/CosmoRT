/* CosmoRT ELF Loader — loads static ELF64 binaries into user address space */

#include "elf.h"
#include "process.h"
#include "serial.h"
#include "config.h"
#include "page_alloc.h"
#include "memops.h"

static void serial_hex64(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar("0123456789abcdef"[(v >> i) & 0xf]);
}

int elf_load(const void *data, size_t len, uint64_t *user_pml4,
             uint64_t stack_top,
             uint64_t *entry, uint64_t *stack_ptr, uint64_t *brk_out) {
    if (len < sizeof(Elf64_Ehdr)) return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Validate ELF magic */
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("elf: bad magic\n");
        return -1;
    }

    /* Must be 64-bit executable for x86_64 */
    if (eh->e_ident[4] != 2) { serial_puts("elf: not 64-bit\n"); return -1; }
    if (eh->e_type != ET_EXEC) { serial_puts("elf: not ET_EXEC\n"); return -1; }
    if (eh->e_machine != EM_X86_64) { serial_puts("elf: not x86_64\n"); return -1; }

    serial_puts("elf: entry="); serial_hex64(eh->e_entry);
    serial_puts(" phnum="); serial_putchar('0' + eh->e_phnum / 10);
    serial_putchar('0' + eh->e_phnum % 10); serial_putchar('\n');

    uint64_t brk_end = 0; /* track highest loaded address for brk */

    /* Process program headers */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (eh->e_phoff + (uint64_t)i * eh->e_phentsize + sizeof(Elf64_Phdr) > len)
            return -1;

        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            ((const uint8_t *)data + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        serial_puts("  LOAD: vaddr="); serial_hex64(ph->p_vaddr);
        serial_puts(" filesz="); serial_hex64(ph->p_filesz);
        serial_puts(" memsz="); serial_hex64(ph->p_memsz);
        serial_putchar('\n');

        /* Map pages for this segment */
        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;

        for (uint64_t va = seg_start; va < seg_end; va += 4096) {
            uint64_t *page = alloc_page(); /* zeroed */
            if (!page) { serial_puts("elf: OOM\n"); return -1; }

            /* Copy file data into this page if applicable */
            if (va < ph->p_vaddr + ph->p_filesz) {
                uint64_t page_start = va;
                uint64_t copy_start = page_start < ph->p_vaddr ? ph->p_vaddr : page_start;
                uint64_t copy_end = page_start + 4096;
                uint64_t file_end = ph->p_vaddr + ph->p_filesz;
                if (copy_end > file_end) copy_end = file_end;

                if (copy_start < copy_end) {
                    uint64_t file_off = ph->p_offset + (copy_start - ph->p_vaddr);
                    uint64_t page_off = copy_start - page_start;
                    uint64_t nbytes = copy_end - copy_start;

                    if (file_off + nbytes <= len)
                        kmemcpy((uint8_t *)page + page_off,
                                (const uint8_t *)data + file_off, nbytes);
                }
            }
            /* BSS (memsz > filesz) is already zeroed by alloc_page */

            if (map_user_page(user_pml4, va, virt_to_phys(page)) < 0) {
                serial_puts("elf: map failed\n");
                return -1;
            }
        }

        /* Track end for brk */
        uint64_t seg_data_end = ph->p_vaddr + ph->p_memsz;
        if (seg_data_end > brk_end) brk_end = seg_data_end;
    }

    /* Set up user stack (map 4 pages at top initially, 16KB) */
    for (int i = 0; i < 4; i++) {
        uint64_t va = stack_top - (uint64_t)(i + 1) * 4096;
        uint64_t *page = alloc_page();
        if (!page) { serial_puts("elf: stack OOM\n"); return -1; }
        if (map_user_page(user_pml4, va, virt_to_phys(page)) < 0)
            return -1;
    }

    /* Build initial stack: argc, argv, envp, auxv
     * We write directly into the physical page at stack top. */
    /* For init process: argc=1, argv={"init", NULL}, envp={NULL}, auxv=minimal */

    /* Find the physical page for stack top */
    /* Stack top is at USER_STACK_TOP - 4096 (the topmost page) */
    /* We just mapped it — the page is the last alloc_page() call for i=0 */
    /* Since we can't easily reverse-lookup, write to the page via kernel identity map.
     * The alloc_page returns a kernel-accessible pointer. Let me re-map stack setup. */

    /* Simpler: allocate a dedicated stack-top page and track its kernel address */
    /* Actually, alloc_page() returns identity-mapped kernel pointer (physical = virtual in kernel).
     * So I can write to it directly. But I already mapped it above. Let me redo the top page. */

    /* The last page mapped was for i=0: va = USER_STACK_TOP - 4096.
     * The alloc_page pointer is lost. Let me track it. */

    /* Redo: allocate stack pages and keep pointer to top page */
    /* Already done above — let me just allocate one more page for the stack setup area,
     * or more practically: put strings + pointers in the second-to-top page region. */

    /* For simplicity: use a minimal stack setup.
     * RSP points to argc on the stack. */
    uint64_t sp = USER_STACK_TOP - 8; /* leave some room */

    /* We need to write into user memory. Since the kernel has identity mapping
     * and the user pages are allocated from identity-mapped arena, we can find
     * the physical page and write to it via its kernel address.
     * But we already lost the pointer. Let's just allocate a fresh stack page
     * and re-map. */

    /* Actually — let me just put a minimal stack. The user process _start doesn't
     * need complex argv for the initial test. We set RSP, argc=0, and that's it. */

    /* Stack layout (Linux ABI):
     * [RSP+0]  = argc
     * [RSP+8]  = argv[0] (NULL for argc=0)
     * [RSP+16] = NULL (envp terminator)
     * [RSP+24] = AT_PAGESZ, 4096
     * [RSP+32] = AT_NULL, 0
     */

    /* To write the stack, find the kernel pointer for the stack page.
     * We'll allocate a new page for the initial stack frame. */
    uint64_t stack_frame_va = stack_top - 4096;
    uint64_t *frame_page = alloc_page();
    if (!frame_page) return -1;
    /* Remap this page (overwrites the previous mapping, which is fine) */
    map_user_page(user_pml4, stack_frame_va, virt_to_phys(frame_page));

    /* Write stack contents at the end of this page */
    /* Page spans [stack_frame_va, stack_frame_va + 4096)
     * We want RSP near the top: stack_frame_va + 4096 - N */
    uint64_t *stk = (uint64_t *)((uint8_t *)frame_page + 4096);

    /* auxv */
    *(--stk) = 0;              /* AT_NULL value */
    *(--stk) = AT_NULL;        /* AT_NULL type */
    *(--stk) = 4096;           /* AT_PAGESZ value */
    *(--stk) = AT_PAGESZ;     /* AT_PAGESZ type */
    *(--stk) = eh->e_entry;   /* AT_ENTRY value */
    *(--stk) = AT_ENTRY;      /* AT_ENTRY type */
    *(--stk) = eh->e_phentsize; /* AT_PHENT value */
    *(--stk) = AT_PHENT;      /* AT_PHENT type */
    *(--stk) = eh->e_phnum;   /* AT_PHNUM value */
    *(--stk) = AT_PHNUM;      /* AT_PHNUM type */
    /* envp */
    *(--stk) = 0;             /* envp[0] = NULL */
    /* argv */
    *(--stk) = 0;             /* argv[0] = NULL */
    /* argc */
    *(--stk) = 0;             /* argc = 0 */

    sp = stack_frame_va + (uint64_t)((uint8_t *)stk - (uint8_t *)frame_page);
    /* Align to 16 bytes (Linux ABI requires RSP % 16 == 0 before _start) */
    sp &= ~0xFULL;

    *entry = eh->e_entry;
    *stack_ptr = sp;
    *brk_out = (brk_end + 0xFFF) & ~0xFFFULL; /* page-aligned */

    serial_puts("elf: loaded, sp="); serial_hex64(sp); serial_putchar('\n');
    return 0;
}
