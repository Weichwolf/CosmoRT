/* COSMO OS UEFI Bootloader — Minimal
 *
 * 1. Get GOP framebuffer
 * 2. Get memory map
 * 3. ExitBootServices
 * 4. Jump to kernel_entry (ASM)
 *
 * After ExitBootServices, NO UEFI calls allowed.
 */

#include <efi.h>
#include <efilib.h>

/* Boot info passed to kernel */
struct boot_info {
    UINT64 fb_addr;
    UINT32 fb_width;
    UINT32 fb_height;
    UINT32 fb_pitch;       /* bytes per scanline */
    UINT32 fb_bpp;
    UINT64 mmap_addr;
    UINT64 mmap_size;
    UINT64 mmap_desc_size;
    UINT64 rsdp_addr;
};

static struct boot_info binfo;
static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID acpi2_guid = ACPI_20_TABLE_GUID;

/* Kernel entry point — in entry.asm */
extern void kernel_entry(struct boot_info *info);

/* Find ACPI RSDP */
static UINT64 find_rsdp(EFI_SYSTEM_TABLE *ST) {
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (CompareGuid(&ST->ConfigurationTable[i].VendorGuid, &acpi2_guid) == 0)
            return (UINT64)ST->ConfigurationTable[i].VendorTable;
    }
    return 0;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

    InitializeLib(ImageHandle, SystemTable);
    ST->ConOut->ClearScreen(ST->ConOut);
    Print(L"COSMO OS Bootloader\r\n");

    /* ── GOP ── */
    status = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: No GOP\r\n");
        BS->Stall(5000000);
        return status;
    }

    binfo.fb_addr   = gop->Mode->FrameBufferBase;
    binfo.fb_width  = gop->Mode->Info->HorizontalResolution;
    binfo.fb_height = gop->Mode->Info->VerticalResolution;
    binfo.fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    binfo.fb_bpp    = 32;
    binfo.rsdp_addr = find_rsdp(SystemTable);

    Print(L"GOP: %dx%d @ 0x%lx\r\n",
          binfo.fb_width, binfo.fb_height, binfo.fb_addr);

    /* ── Memory Map ── */
    UINTN mmap_size = 0, map_key, desc_size;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;

    /* First call: get size */
    BS->GetMemoryMap(&mmap_size, NULL, &map_key, &desc_size, &desc_ver);
    mmap_size += 4 * desc_size; /* padding for the allocation itself */

    status = BS->AllocatePool(EfiLoaderData, mmap_size, (void **)&mmap);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: AllocatePool\r\n");
        return status;
    }

    status = BS->GetMemoryMap(&mmap_size, mmap, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        Print(L"FATAL: GetMemoryMap\r\n");
        return status;
    }

    binfo.mmap_addr      = (UINT64)mmap;
    binfo.mmap_size      = mmap_size;
    binfo.mmap_desc_size = desc_size;

    Print(L"Memory map: %d entries\r\n", mmap_size / desc_size);
    Print(L"Calling ExitBootServices...\r\n");

    /* ── ExitBootServices ── */
    status = BS->ExitBootServices(ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Memory map changed — retry */
        mmap_size += 4 * desc_size;
        BS->GetMemoryMap(&mmap_size, mmap, &map_key, &desc_size, &desc_ver);
        status = BS->ExitBootServices(ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            /* Can't print anymore if first ExitBootServices partially succeeded */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* ── NO UEFI CALLS BEYOND THIS POINT ── */

    /* Jump to kernel */
    kernel_entry(&binfo);

    /* Never reached */
    for (;;) __asm__ volatile("hlt");
    return EFI_SUCCESS;
}
