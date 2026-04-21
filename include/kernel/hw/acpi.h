/* CosmoRT ACPI table parser — minimal, no AML/ACPICA.
 *
 * RSDP → XSDT (rev >= 2) or RSDT (rev 0) → SDT lookup by 4-byte signature.
 * Caller gets mapped (direct-map) pointers. All structs are spec layout with
 * `packed` — never add fields, never reorder.
 */
#ifndef HW_ACPI_H
#define HW_ACPI_H

#include <stdint.h>
#include <stddef.h>

#define ACPI_SIG_RSDP   "RSD PTR "       /* 8 bytes, trailing space */
#define ACPI_SIG_RSDT   "RSDT"
#define ACPI_SIG_XSDT   "XSDT"
#define ACPI_SIG_FADT   "FACP"           /* Fixed ACPI Description Table */
#define ACPI_SIG_HPET   "HPET"
#define ACPI_SIG_MADT   "APIC"           /* Multiple APIC Description Table */
#define ACPI_SIG_MCFG   "MCFG"

#define ACPI_REV_1      0                /* ACPI 1.0 — RSDT only */
#define ACPI_REV_2      2                /* ACPI 2.0+ — XSDT present */

#define ACPI_SIG_LEN    4
#define ACPI_RSDP_SIG_LEN 8

#define ACPI_MADT_TYPE_LAPIC         0
#define ACPI_MADT_TYPE_IOAPIC        1
#define ACPI_MADT_TYPE_INT_OVERRIDE  2
#define ACPI_MADT_TYPE_NMI_SOURCE    3
#define ACPI_MADT_TYPE_LAPIC_NMI     4
#define ACPI_MADT_TYPE_LAPIC_ADDR    5
#define ACPI_MADT_TYPE_X2APIC        9

struct acpi_rsdp {
    char     signature[ACPI_RSDP_SIG_LEN];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* revision >= 2 fields */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[ACPI_SIG_LEN];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_gas {
    uint8_t  address_space_id;        /* 0=SystemMemory, 1=SystemIO */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;          /* PM_TMR I/O port */
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;         /* 4 if 32-bit, else 24-bit */
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved1;
    uint32_t flags;                   /* ACPI_FADT_FLAG_* */
} __attribute__((packed));

#define ACPI_FADT_FLAG_TMR_VAL_EXT   (1u << 8)  /* PM_TMR is 32-bit */

struct acpi_hpet {
    struct acpi_sdt_header header;
    uint32_t event_timer_block_id;    /* hardware rev, num comparators, counter size */
    struct acpi_gas base_address;
    uint8_t  hpet_number;
    uint16_t min_clock_tick;
    uint8_t  page_protection;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    /* variable-length entries follow: each starts with type+length byte pair */
} __attribute__((packed));

struct acpi_madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct acpi_madt_lapic {
    struct acpi_madt_entry entry;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;                   /* bit 0 = enabled */
} __attribute__((packed));

struct acpi_madt_ioapic {
    struct acpi_madt_entry entry;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_irq_base;
} __attribute__((packed));

/* ── Parser API ─────────────────────────────────────── */

void acpi_init(void);

/* 1-byte sum over `len` bytes must equal 0. */
int  acpi_checksum_ok(const void *buf, size_t len);

/* Parses a candidate RSDP buffer (no mapping, just layout check). */
int  acpi_rsdp_validate(const struct acpi_rsdp *r);

/* Walks the root table (XSDT if rev >= 2, else RSDT). Returns mapped pointer
 * to first table whose signature matches `sig` (4 chars) and checksum is
 * valid, or NULL. */
const struct acpi_sdt_header *acpi_find_table(const char *sig);

/* MADT helpers: count enabled LAPIC / IOAPIC entries. Returns -1 if no MADT. */
int acpi_madt_count(uint8_t entry_type);

/* Returns the current RSDP pointer, or NULL if acpi_init has not discovered
 * a valid RSDP. */
const struct acpi_rsdp *acpi_rsdp(void);

/* For unit tests: force parser to use a caller-supplied RSDP phys address and
 * re-scan the root table. Returns 0 on success. Previous state is replaced. */
int acpi_override_rsdp_for_test(uint64_t phys);

/* For unit tests: restore parser state from boot_info after an override. */
void acpi_restore_from_boot(void);

#endif
