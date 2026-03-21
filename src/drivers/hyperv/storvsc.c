/* CosmoRT storvsc — Hyper-V Synthetic SCSI Storage Driver
 *
 * SCSI commands over VMBus. Replaces virtio-blk on Hyper-V.
 * Implements blk_read/blk_write/blk_capacity for CosmoFS.
 *
 * Only imports: hw.h, config.h, serial.h, vmbus.h
 */

#include "vmbus.h"
#include "hw.h"
#include "config.h"
#include "serial.h"

/* StorVSC device GUID: {ba6163d9-04a1-4d29-b605-72e2ffb1dc7f} */
static const uint8_t STORVSC_GUID[16] = {
    0xd9, 0x63, 0x61, 0xba, 0xa1, 0x04, 0x29, 0x4d,
    0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f
};

/* ---- VSTOR protocol ---- */

#define VSTOR_CURRENT_VERSION       0x0002  /* VHDX-compatible */

/* Operation types */
#define VSTOR_OP_COMPLETE           1
#define VSTOR_OP_REMOVE_DEVICE      2
#define VSTOR_OP_EXECUTE_SRB        3
#define VSTOR_OP_RESET_LUN          4
#define VSTOR_OP_RESET_ADAPTER      5
#define VSTOR_OP_RESET_BUS          6
#define VSTOR_OP_BEGIN_INIT         7
#define VSTOR_OP_END_INIT           8
#define VSTOR_OP_QUERY_PROTOCOL_VER 9
#define VSTOR_OP_QUERY_PROPERTIES   10
#define VSTOR_OP_ENUMERATE_BUS      11
#define VSTOR_OP_FCHBA_DATA         12
#define VSTOR_OP_CREATE_MULTICH     13

/* SCSI CDB opcodes */
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ_10         0x28
#define SCSI_WRITE_10        0x2A
#define SCSI_TEST_UNIT_READY 0x00

/* SRB flags */
#define SRB_FLAGS_DATA_IN    0x00000040
#define SRB_FLAGS_DATA_OUT   0x00000080
#define SRB_FLAGS_NO_DATA    0x00000000

/* VSTOR packet (sent over VMBus ring) */
struct vstor_packet {
    uint32_t operation;
    uint32_t flags;
    uint32_t status;

    /* SRB subset */
    uint8_t  path_id;
    uint8_t  target_id;
    uint8_t  lun;
    uint8_t  cdb_len;
    uint8_t  sense_len;
    uint8_t  srb_status;
    uint8_t  scsi_status;
    uint8_t  reserved1;

    uint32_t data_transfer_len;
    uint32_t srb_flags;

    uint8_t  cdb[16];
    uint8_t  sense_data[20];

    /* Data buffer GPA (guest physical address) for DMA */
    uint64_t data_buffer;
    uint32_t data_buffer_len;
    uint32_t reserved2;
} __attribute__((packed));

/* ---- State ---- */

static struct vmbus_channel *stor_ch;
static volatile int cmd_done;
static struct vstor_packet cmd_resp;
static hw_spinlock_t stor_lock = HW_SPINLOCK_INIT;

/* I/O DMA buffer */
#define STOR_DMA_SIZE  4096
static void    *stor_dma_virt;
static uint64_t stor_dma_phys;

/* Capacity */
static uint64_t stor_total_blocks;  /* in 512-byte sectors */
static uint32_t stor_block_size;    /* bytes per sector */

/* ---- Callback ---- */

static void storvsc_callback(struct vmbus_channel *ch, void *ctx) {
    (void)ctx;
    uint64_t pkt_type;
    uint8_t buf[256];
    int len = vmbus_recv_pkt(ch, &pkt_type, buf, sizeof(buf));
    if (len >= (int)sizeof(struct vstor_packet)) {
        hw_memcpy(&cmd_resp, buf, sizeof(cmd_resp));
        cmd_done = 1;
    }
}

/* ---- Command helpers ---- */

static int vstor_send_cmd(struct vstor_packet *pkt) {
    cmd_done = 0;
    int r = vmbus_send_pkt(stor_ch, VMBUS_PKT_DATA_INBAND,
                            pkt, sizeof(*pkt), 0, 0);
    if (r < 0) return -1;

    /* Wait for response */
    uint64_t deadline = hw_ms() + 5000;
    while (!cmd_done) {
        if (hw_ms() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

static int vstor_init_protocol(void) {
    struct vstor_packet pkt;
    hw_memset(&pkt, 0, sizeof(pkt));

    /* BEGIN_INIT */
    pkt.operation = VSTOR_OP_BEGIN_INIT;
    if (vstor_send_cmd(&pkt) < 0) return -1;

    /* QUERY_PROTOCOL_VERSION */
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_QUERY_PROTOCOL_VER;
    pkt.flags = VSTOR_CURRENT_VERSION;
    if (vstor_send_cmd(&pkt) < 0) return -1;

    /* QUERY_PROPERTIES */
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_QUERY_PROPERTIES;
    if (vstor_send_cmd(&pkt) < 0) return -1;

    /* END_INIT */
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_END_INIT;
    if (vstor_send_cmd(&pkt) < 0) return -1;

    return 0;
}

static int scsi_read_capacity(void) {
    /* Allocate DMA buffer for capacity data (8 bytes) */
    uint8_t cap_buf[8];

    struct vstor_packet pkt;
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_EXECUTE_SRB;
    pkt.path_id = 0;
    pkt.target_id = 0;
    pkt.lun = 0;
    pkt.cdb_len = 10;
    pkt.srb_flags = SRB_FLAGS_DATA_IN;
    pkt.data_transfer_len = 8;
    pkt.data_buffer = stor_dma_phys;
    pkt.data_buffer_len = 8;
    pkt.cdb[0] = SCSI_READ_CAPACITY;

    if (vstor_send_cmd(&pkt) < 0) return -1;

    hw_memcpy(cap_buf, stor_dma_virt, 8);

    /* Parse: big-endian LBA (4 bytes) + block size (4 bytes) */
    uint32_t last_lba = ((uint32_t)cap_buf[0] << 24) |
                        ((uint32_t)cap_buf[1] << 16) |
                        ((uint32_t)cap_buf[2] << 8)  |
                        cap_buf[3];
    stor_block_size = ((uint32_t)cap_buf[4] << 24) |
                      ((uint32_t)cap_buf[5] << 16) |
                      ((uint32_t)cap_buf[6] << 8)  |
                      cap_buf[7];

    stor_total_blocks = (uint64_t)last_lba + 1;

    serial_puts("storvsc: capacity ");
    serial_hex64(stor_total_blocks);
    serial_puts(" sectors, ");
    serial_hex64(stor_block_size);
    serial_puts(" bytes/sector\n");

    return 0;
}

/* ---- Public API ---- */

int storvsc_init(void) {
    stor_ch = vmbus_find_channel(STORVSC_GUID);
    if (!stor_ch) {
        serial_puts("storvsc: no channel found\n");
        return -1;
    }
    serial_puts("storvsc: channel found, relid=");
    serial_hex64(stor_ch->child_relid);
    serial_putchar('\n');

    if (vmbus_open(stor_ch, 64 * 1024, storvsc_callback, 0) < 0) {
        serial_puts("storvsc: open failed\n");
        return -1;
    }

    /* Allocate DMA buffer for SCSI data */
    if (cosmo_dma_alloc(STOR_DMA_SIZE, &stor_dma_virt, &stor_dma_phys) < 0) {
        serial_puts("storvsc: DMA alloc failed\n");
        return -1;
    }

    /* Initialize VSTOR protocol */
    if (vstor_init_protocol() < 0) {
        serial_puts("storvsc: protocol init failed\n");
        return -1;
    }

    /* Read capacity */
    if (scsi_read_capacity() < 0) {
        serial_puts("storvsc: read capacity failed\n");
        return -1;
    }

    serial_puts("storvsc: ready\n");
    return 0;
}

int storvsc_read(uint64_t block, void *buf) {
    if (!stor_ch || stor_ch->state != 1) return -1;

    uint64_t flags;
    hw_spin_lock_irq(&stor_lock, &flags);

    /* Convert 4KB block to 512-byte sectors */
    uint32_t sector = (uint32_t)(block * (4096 / stor_block_size));
    uint32_t count = (uint32_t)(4096 / stor_block_size);

    struct vstor_packet pkt;
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_EXECUTE_SRB;
    pkt.cdb_len = 10;
    pkt.srb_flags = SRB_FLAGS_DATA_IN;
    pkt.data_transfer_len = 4096;
    pkt.data_buffer = stor_dma_phys;
    pkt.data_buffer_len = 4096;

    /* SCSI READ(10) CDB */
    pkt.cdb[0] = SCSI_READ_10;
    pkt.cdb[2] = (uint8_t)(sector >> 24);
    pkt.cdb[3] = (uint8_t)(sector >> 16);
    pkt.cdb[4] = (uint8_t)(sector >> 8);
    pkt.cdb[5] = (uint8_t)(sector);
    pkt.cdb[7] = (uint8_t)(count >> 8);
    pkt.cdb[8] = (uint8_t)(count);

    int r = vstor_send_cmd(&pkt);
    if (r == 0)
        hw_memcpy(buf, stor_dma_virt, 4096);

    hw_spin_unlock_irq(&stor_lock, flags);
    return r;
}

int storvsc_write(uint64_t block, const void *buf) {
    if (!stor_ch || stor_ch->state != 1) return -1;

    uint64_t flags;
    hw_spin_lock_irq(&stor_lock, &flags);

    uint32_t sector = (uint32_t)(block * (4096 / stor_block_size));
    uint32_t count = (uint32_t)(4096 / stor_block_size);

    hw_memcpy(stor_dma_virt, buf, 4096);

    struct vstor_packet pkt;
    hw_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OP_EXECUTE_SRB;
    pkt.cdb_len = 10;
    pkt.srb_flags = SRB_FLAGS_DATA_OUT;
    pkt.data_transfer_len = 4096;
    pkt.data_buffer = stor_dma_phys;
    pkt.data_buffer_len = 4096;

    /* SCSI WRITE(10) CDB */
    pkt.cdb[0] = SCSI_WRITE_10;
    pkt.cdb[2] = (uint8_t)(sector >> 24);
    pkt.cdb[3] = (uint8_t)(sector >> 16);
    pkt.cdb[4] = (uint8_t)(sector >> 8);
    pkt.cdb[5] = (uint8_t)(sector);
    pkt.cdb[7] = (uint8_t)(count >> 8);
    pkt.cdb[8] = (uint8_t)(count);

    int r = vstor_send_cmd(&pkt);

    hw_spin_unlock_irq(&stor_lock, flags);
    return r;
}

uint64_t storvsc_capacity(void) {
    if (!stor_block_size) return 0;
    /* Return capacity in 4KB blocks */
    return (stor_total_blocks * stor_block_size) / 4096;
}
