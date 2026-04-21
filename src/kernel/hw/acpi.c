/* CosmoRT ACPI table parser.
 *
 * Flow: boot_info.rsdp_addr (physical, from UEFI config tables) →
 *       phys_to_virt → validate signature + checksum → pick XSDT or RSDT
 *       by revision → walk entries, filter by signature.
 *
 * No ACPICA, no AML — just table-layout reads. Every returned pointer is
 * already in the kernel direct map.
 */

#include "hw/acpi.h"
#include "boot_info.h"
#include "config.h"
#include "hw/serial.h"
#include "memops.h"

extern struct boot_info *g_boot_info;

static const struct acpi_rsdp       *g_rsdp;
static const struct acpi_sdt_header *g_root;     /* XSDT or RSDT header */
static int                           g_root_is_xsdt;

int acpi_checksum_ok(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static int sig4_eq(const char *a, const char *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

int acpi_rsdp_validate(const struct acpi_rsdp *r) {
    if (!r) return 0;
    static const char sig[ACPI_RSDP_SIG_LEN] = { 'R','S','D',' ','P','T','R',' ' };
    for (int i = 0; i < ACPI_RSDP_SIG_LEN; i++)
        if (r->signature[i] != sig[i]) return 0;
    /* revision 0: checksum covers first 20 bytes only */
    if (!acpi_checksum_ok(r, 20)) return 0;
    if (r->revision >= ACPI_REV_2) {
        if (r->length < sizeof(*r)) return 0;
        if (!acpi_checksum_ok(r, r->length)) return 0;
    }
    return 1;
}

static const struct acpi_sdt_header *sdt_map(uint64_t phys) {
    if (!phys) return 0;
    return (const struct acpi_sdt_header *)phys_to_virt(phys);
}

static int sdt_valid(const struct acpi_sdt_header *h) {
    if (!h) return 0;
    if (h->length < sizeof(*h)) return 0;
    return acpi_checksum_ok(h, h->length);
}

static void load_root(void) {
    g_root = 0;
    g_root_is_xsdt = 0;
    if (!g_rsdp) return;

    if (g_rsdp->revision >= ACPI_REV_2 && g_rsdp->xsdt_address) {
        const struct acpi_sdt_header *h = sdt_map(g_rsdp->xsdt_address);
        if (sdt_valid(h) && sig4_eq(h->signature, ACPI_SIG_XSDT)) {
            g_root = h;
            g_root_is_xsdt = 1;
            return;
        }
    }
    if (g_rsdp->rsdt_address) {
        const struct acpi_sdt_header *h = sdt_map((uint64_t)g_rsdp->rsdt_address);
        if (sdt_valid(h) && sig4_eq(h->signature, ACPI_SIG_RSDT))
            g_root = h;
    }
}

const struct acpi_sdt_header *acpi_find_table(const char *sig) {
    if (!g_root || !sig) return 0;

    uint32_t body = g_root->length - (uint32_t)sizeof(*g_root);
    const uint8_t *entries = (const uint8_t *)(g_root + 1);

    if (g_root_is_xsdt) {
        uint32_t n = body / 8;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t phys;
            kmemcpy(&phys, entries + i * 8, sizeof(phys));
            const struct acpi_sdt_header *h = sdt_map(phys);
            if (sdt_valid(h) && sig4_eq(h->signature, sig)) return h;
        }
    } else {
        uint32_t n = body / 4;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t phys;
            kmemcpy(&phys, entries + i * 4, sizeof(phys));
            const struct acpi_sdt_header *h = sdt_map((uint64_t)phys);
            if (sdt_valid(h) && sig4_eq(h->signature, sig)) return h;
        }
    }
    return 0;
}

int acpi_madt_count(uint8_t entry_type) {
    const struct acpi_sdt_header *h = acpi_find_table(ACPI_SIG_MADT);
    if (!h) return -1;

    const struct acpi_madt *m = (const struct acpi_madt *)h;
    const uint8_t *p   = (const uint8_t *)m + sizeof(*m);
    const uint8_t *end = (const uint8_t *)m + m->header.length;
    int count = 0;

    while (p + sizeof(struct acpi_madt_entry) <= end) {
        const struct acpi_madt_entry *e = (const struct acpi_madt_entry *)p;
        if (e->length < sizeof(*e) || p + e->length > end) break;
        if (e->type == entry_type) {
            if (entry_type == ACPI_MADT_TYPE_LAPIC &&
                e->length >= sizeof(struct acpi_madt_lapic)) {
                const struct acpi_madt_lapic *la = (const struct acpi_madt_lapic *)p;
                if (la->flags & 1u) count++;
            } else {
                count++;
            }
        }
        p += e->length;
    }
    return count;
}

const struct acpi_rsdp *acpi_rsdp(void) { return g_rsdp; }

static void log_found(const char *sig) {
    if (acpi_find_table(sig)) {
        serial_puts("acpi: ");
        serial_puts(sig);
        serial_puts("\n");
    }
}

__attribute__((cold))
void acpi_init(void) {
    g_rsdp = 0;
    g_root = 0;
    g_root_is_xsdt = 0;

    if (!g_boot_info || !g_boot_info->rsdp_addr) {
        serial_puts("acpi: no RSDP from firmware\n");
        return;
    }

    const struct acpi_rsdp *r = (const struct acpi_rsdp *)phys_to_virt(g_boot_info->rsdp_addr);
    if (!acpi_rsdp_validate(r)) {
        serial_puts("acpi: RSDP invalid\n");
        return;
    }

    g_rsdp = r;
    load_root();

    if (!g_root) {
        serial_puts("acpi: no valid root table\n");
        return;
    }

    serial_puts(g_root_is_xsdt ? "acpi: XSDT\n" : "acpi: RSDT\n");
    log_found(ACPI_SIG_FADT);
    log_found(ACPI_SIG_HPET);
    log_found(ACPI_SIG_MADT);
    log_found(ACPI_SIG_MCFG);
}

int acpi_override_rsdp_for_test(uint64_t phys) {
    const struct acpi_rsdp *r = (const struct acpi_rsdp *)phys_to_virt(phys);
    if (!acpi_rsdp_validate(r)) return -1;
    g_rsdp = r;
    load_root();
    return 0;
}

void acpi_restore_from_boot(void) {
    g_rsdp = 0;
    g_root = 0;
    g_root_is_xsdt = 0;
    if (!g_boot_info || !g_boot_info->rsdp_addr) return;
    const struct acpi_rsdp *r = (const struct acpi_rsdp *)phys_to_virt(g_boot_info->rsdp_addr);
    if (!acpi_rsdp_validate(r)) return;
    g_rsdp = r;
    load_root();
}
