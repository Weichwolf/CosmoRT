/* test: SHA-256 correctness (NIST FIPS 180-4 vectors) + rt_hash queue */
#include "ktest.h"
#include "cosmort.h"

#define RT_QUERY_HASH_REQUEST  9
#define RT_QUERY_HASH_RESULT  10

static void msleep(int ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* Compare hash against hex string (64 chars) */
static int hash_eq(const uint8_t h[32], const char *hex) {
    for (int i = 0; i < 32; i++) {
        int hi = hex[2*i], lo = hex[2*i+1];
        hi = (hi >= 'a') ? hi - 'a' + 10 : hi - '0';
        lo = (lo >= 'a') ? lo - 'a' + 10 : lo - '0';
        if (h[i] != (uint8_t)((hi << 4) | lo)) return 0;
    }
    return 1;
}

static void put_hash(const uint8_t h[32]) {
    static const char hx[] = "0123456789abcdef";
    char buf[65];
    for (int i = 0; i < 32; i++) {
        buf[2*i]   = hx[h[i] >> 4];
        buf[2*i+1] = hx[h[i] & 0xf];
    }
    buf[64] = 0;
    puts(buf);
}

/* ── SHA-256 via rt_hash syscall ────────────────── */

static long hash_request(uint64_t addr, uint32_t len, uint64_t cookie) {
    return sc4(SYS_COSMO_RT_QUERY, RT_QUERY_HASH_REQUEST,
               (long)addr, (long)len, (long)cookie);
}

static long hash_result(uint64_t *cookie, uint8_t hash[32]) {
    return sc4(SYS_COSMO_RT_QUERY, RT_QUERY_HASH_RESULT,
               (long)cookie, (long)hash, 0);
}

/* ── Test vectors ────────────────────────────────── */

static void test_sha256(void) {
    puts("\n[sha256]\n");

    /* Test 1: empty string → e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    {
        uint64_t cookie = 0;
        uint8_t hash[32];
        static const char empty = '\0';

        long r = hash_request((uint64_t)(uintptr_t)&empty, 0, 0x100);
        check_val("sha256 empty: request", r, 0);

        /* Wait for RT-Core to process */
        msleep(300);

        r = hash_result(&cookie, hash);
        check_val("sha256 empty: result", r, 0);
        check_val("sha256 empty: cookie", (long)cookie, 0x100);

        int ok = hash_eq(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        if (!ok) { puts("  got: "); put_hash(hash); puts("\n"); }
        check("sha256 empty: hash", ok);
    }

    /* Test 2: "abc" → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    {
        uint64_t cookie = 0;
        uint8_t hash[32];
        static const char abc[] = "abc";

        long r = hash_request((uint64_t)(uintptr_t)abc, 3, 0x200);
        check_val("sha256 abc: request", r, 0);
        msleep(300);

        r = hash_result(&cookie, hash);
        check_val("sha256 abc: result", r, 0);
        check_val("sha256 abc: cookie", (long)cookie, 0x200);

        int ok = hash_eq(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        if (!ok) { puts("  got: "); put_hash(hash); puts("\n"); }
        check("sha256 abc: hash", ok);
    }

    /* Test 3: 1000000 × "a" → cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0 */
    {
        /* Allocate 1MB buffer */
        long buf = sc6(SYS_MMAP, 0, 1000000, PROT_RW, MAP_PRIV_ANON, -1, 0);
        check("sha256 1M: mmap", buf > 0);
        if (buf > 0) {
            char *p = (char *)buf;
            for (int i = 0; i < 1000000; i++) p[i] = 'a';

            uint64_t cookie = 0;
            uint8_t hash[32];
            long r = hash_request((uint64_t)(uintptr_t)p, 1000000, 0x300);
            check_val("sha256 1M: request", r, 0);
            msleep(500);

            r = hash_result(&cookie, hash);
            check_val("sha256 1M: result", r, 0);
            check_val("sha256 1M: cookie", (long)cookie, 0x300);

            int ok = hash_eq(hash, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
            if (!ok) { puts("  got: "); put_hash(hash); puts("\n"); }
            check("sha256 1M: hash", ok);

            sc2(SYS_MUNMAP, buf, 1000000);
        }
    }

    /* Test 4: 4096 × 0x00 → ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7 */
    {
        long buf = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        check("sha256 4K zero: mmap", buf > 0);
        if (buf > 0) {
            /* MAP_ANONYMOUS pages are already zeroed */
            uint64_t cookie = 0;
            uint8_t hash[32];
            long r = hash_request((uint64_t)(uintptr_t)buf, 4096, 0x400);
            check_val("sha256 4K zero: request", r, 0);
            msleep(300);

            r = hash_result(&cookie, hash);
            check_val("sha256 4K zero: result", r, 0);
            check_val("sha256 4K zero: cookie", (long)cookie, 0x400);

            int ok = hash_eq(hash, "ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7");
            if (!ok) { puts("  got: "); put_hash(hash); puts("\n"); }
            check("sha256 4K zero: hash", ok);

            sc2(SYS_MUNMAP, buf, 4096);
        }
    }
}

TEST("sha256", test_sha256);
