#include "ktest.h"

/* rt_channel is header-only (static inline), include directly.
 * arch.h → arch/x86_64.h provides arch_store_release/arch_load_acquire. */
#include "rt.h"

static uint8_t chbuf[256] __attribute__((aligned(64)));

static void test_rt_channel(void) {
    puts("\n[RT Channel]\n");

    rt_channel_t ch;

    /* ── init ── */
    check_val("init power-of-2", (long)rt_channel_init(&ch, chbuf, 256), 0);
    check_val("init non-pow2 fails", (long)rt_channel_init(&ch, chbuf, 100), -1);
    rt_channel_init(&ch, chbuf, 256);

    /* ── empty pop ── */
    uint8_t tmp[64];
    check_val("pop empty returns -1", (long)rt_channel_pop(&ch, tmp, sizeof(tmp)), -1);

    /* ── push/pop roundtrip ── */
    const char *msg = "hello";
    check_val("push succeeds", (long)rt_channel_push(&ch, msg, 5), 0);
    check_val("used after push", (long)rt_channel_used(&ch), 9);  /* 4 header + 5 payload */

    char out[32];
    int len = rt_channel_pop(&ch, out, sizeof(out));
    check_val("pop returns len=5", (long)len, 5);
    check("pop data matches", out[0]=='h' && out[1]=='e' && out[2]=='l'
                             && out[3]=='l' && out[4]=='o');
    check_val("used after pop", (long)rt_channel_used(&ch), 0);

    /* ── multiple messages ── */
    rt_channel_init(&ch, chbuf, 256);
    const char *m1 = "AAA", *m2 = "BB", *m3 = "C";
    check_val("push m1", (long)rt_channel_push(&ch, m1, 3), 0);
    check_val("push m2", (long)rt_channel_push(&ch, m2, 2), 0);
    check_val("push m3", (long)rt_channel_push(&ch, m3, 1), 0);

    char o1[8], o2[8], o3[8];
    check_val("pop m1 len", (long)rt_channel_pop(&ch, o1, sizeof(o1)), 3);
    check("pop m1 data", o1[0]=='A' && o1[1]=='A' && o1[2]=='A');
    check_val("pop m2 len", (long)rt_channel_pop(&ch, o2, sizeof(o2)), 2);
    check("pop m2 data", o2[0]=='B' && o2[1]=='B');
    check_val("pop m3 len", (long)rt_channel_pop(&ch, o3, sizeof(o3)), 1);
    check("pop m3 data", o3[0]=='C');
    check_val("empty after 3 pops", (long)rt_channel_pop(&ch, tmp, sizeof(tmp)), -1);

    /* ── full buffer ── */
    rt_channel_init(&ch, chbuf, 64);
    /* 64 bytes total. Each msg = 4+8=12 bytes. 5 msgs = 60 bytes, fits. 6th won't. */
    uint8_t payload[8];
    for (int i = 0; i < 8; i++) payload[i] = (uint8_t)i;
    int pushed = 0;
    for (int i = 0; i < 10; i++) {
        if (rt_channel_push(&ch, payload, 8) == 0) pushed++;
        else break;
    }
    check_val("pushed until full", (long)pushed, 5);  /* 5 * 12 = 60 <= 64 */
    check_val("push on full returns -1",
              (long)rt_channel_push(&ch, payload, 8), -1);

    /* ── wrap-around ── */
    rt_channel_init(&ch, chbuf, 32);
    /* Push 12 bytes (4+8), pop, push again — forces wrap */
    uint8_t d8[8] = {1,2,3,4,5,6,7,8};
    check_val("wrap push 1", (long)rt_channel_push(&ch, d8, 8), 0);
    check_val("wrap pop 1", (long)rt_channel_pop(&ch, tmp, sizeof(tmp)), 8);
    /* head=12, tail=12. Push another 12 → head=24. Another → head=36 wraps in 32-byte buf. */
    check_val("wrap push 2", (long)rt_channel_push(&ch, d8, 8), 0);
    check_val("wrap push 3 (wraps)", (long)rt_channel_push(&ch, d8, 4), 0);
    /* Pop and verify data integrity across wrap boundary */
    uint8_t v1[8], v2[8];
    check_val("wrap pop 2", (long)rt_channel_pop(&ch, v1, sizeof(v1)), 8);
    check("wrap data 2 intact", v1[0]==1 && v1[7]==8);
    check_val("wrap pop 3", (long)rt_channel_pop(&ch, v2, sizeof(v2)), 4);
    check("wrap data 3 intact", v2[0]==1 && v2[3]==4);
}

TEST("rt_channel", test_rt_channel);
