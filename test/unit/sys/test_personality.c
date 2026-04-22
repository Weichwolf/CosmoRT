#include "ktest.h"

/* personality(2): Query mit 0xFFFFFFFF liefert aktuelle Persona ohne Update.
 * Setzen mit beliebigem Wert akzeptiert, gibt alte Persona zurueck.
 * fork inherits — hier ktest-Scope: Query-Roundtrip + Restore. */

#define READ_IMPLIES_EXEC 0x0400000UL
#define ADDR_NO_RANDOMIZE 0x0040000UL
#define PER_LINUX         0x0UL

static void test_personality(void) {
    puts("\n[personality]\n");

    long cur = sc5(SYS_PERSONALITY, 0xFFFFFFFFL, 0, 0, 0, 0);
    check("query does not fail", cur >= 0);

    long prev = sc5(SYS_PERSONALITY, READ_IMPLIES_EXEC, 0, 0, 0, 0);
    check_val("set READ_IMPLIES_EXEC returns old", prev, cur);

    long now = sc5(SYS_PERSONALITY, 0xFFFFFFFFL, 0, 0, 0, 0);
    check_val("query after set matches", now, (long)READ_IMPLIES_EXEC);

    long prev2 = sc5(SYS_PERSONALITY, ADDR_NO_RANDOMIZE, 0, 0, 0, 0);
    check_val("set ADDR_NO_RANDOMIZE returns READ_IMPLIES_EXEC",
              prev2, (long)READ_IMPLIES_EXEC);

    long prev3 = sc5(SYS_PERSONALITY, PER_LINUX, 0, 0, 0, 0);
    check_val("restore PER_LINUX returns ADDR_NO_RANDOMIZE",
              prev3, (long)ADDR_NO_RANDOMIZE);
}

TEST("personality", test_personality);
