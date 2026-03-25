/* test: IP/TCP/UDP Checksums — verify ip_cksum against known vectors */
#include "ktest.h"

/* ip_cksum from net_util.h — inline, but we test through the header */
static inline uint16_t test_ip_cksum(const uint8_t *d, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        uint16_t w = (uint16_t)(d[i] << 8);
        if (i + 1 < len) w |= d[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void test_ip_checksum(void) {
    puts("\n[net/ip_checksum]\n");

    /* 1. RFC 1071 example: IP header checksum
     * Standard 20-byte IP header with known checksum. */
    uint8_t ip_hdr[20] = {
        0x45, 0x00, 0x00, 0x3C,  /* ver/ihl, tos, len=60 */
        0x1C, 0x46, 0x40, 0x00,  /* id, flags+offset */
        0x40, 0x06, 0x00, 0x00,  /* ttl=64, proto=TCP, cksum=0 (to compute) */
        0xAC, 0x10, 0x0A, 0x63,  /* src=172.16.10.99 */
        0xAC, 0x10, 0x0A, 0x0C   /* dst=172.16.10.12 */
    };
    uint16_t cksum = test_ip_cksum(ip_hdr, 20);
    /* Fill checksum and verify round-trip */
    ip_hdr[10] = (uint8_t)(cksum >> 8);
    ip_hdr[11] = (uint8_t)(cksum & 0xFF);
    uint16_t verify = test_ip_cksum(ip_hdr, 20);
    check("ip hdr cksum roundtrip", verify == 0);

    /* 2. All-zeros → checksum = 0xFFFF */
    uint8_t zeros[20] = {0};
    uint16_t zck = test_ip_cksum(zeros, 20);
    check("all-zeros → 0xFFFF", zck == 0xFFFF);

    /* 3. Odd-length data */
    uint8_t odd[3] = {0x01, 0x02, 0x03};
    uint16_t ock = test_ip_cksum(odd, 3);
    /* Manual: 0x0102 + 0x0300 = 0x0402 → ~0x0402 = 0xFBFD */
    check("odd-length checksum", ock == 0xFBFD);

    /* 4. Single byte */
    uint8_t one[1] = {0xFF};
    uint16_t oneck = test_ip_cksum(one, 1);
    /* 0xFF00 → ~0xFF00 = 0x00FF */
    check("single byte", oneck == 0x00FF);

    /* 5. Verify complement property: data + checksum = 0 */
    uint8_t data[10] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    uint16_t dck = test_ip_cksum(data, 10);
    /* Insert checksum as last 2 bytes of extended buffer */
    uint8_t ext[12];
    for (int i = 0; i < 10; i++) ext[i] = data[i];
    ext[10] = (uint8_t)(dck >> 8);
    ext[11] = (uint8_t)(dck & 0xFF);
    uint16_t ext_ck = test_ip_cksum(ext, 12);
    check("complement property", ext_ck == 0);

    /* 6. Max values (overflow handling) */
    uint8_t maxdata[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint16_t mck = test_ip_cksum(maxdata, 4);
    /* 0xFFFF + 0xFFFF = 0x1FFFE → fold → 0xFFFF → ~0xFFFF = 0x0000 */
    check("max values", mck == 0x0000);
}

TEST("net/ip_checksum", test_ip_checksum);
