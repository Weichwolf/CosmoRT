#include "ktest.h"

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

static void test_winsize(void) {
    puts("\n[winsize]\n");

    /* TIOCGWINSZ on stdin (fd 0 = PTY slave in ktest) */
    struct winsize ws = {0};
    long r = sc3(SYS_IOCTL, 0, TIOCGWINSZ, (long)&ws);
    check_val("TIOCGWINSZ returns 0", r, 0);
    check("ws_row > 0", ws.ws_row > 0);
    check("ws_col > 0", ws.ws_col > 0);
    check("ws_col in range 80-200", ws.ws_col >= 80 && ws.ws_col <= 200);
    check("ws_row in range 24-60", ws.ws_row >= 24 && ws.ws_row <= 60);

    /* TIOCSWINSZ → TIOCGWINSZ roundtrip */
    struct winsize set = { .ws_row = 50, .ws_col = 132, .ws_xpixel = 0, .ws_ypixel = 0 };
    r = sc3(SYS_IOCTL, 0, TIOCSWINSZ, (long)&set);
    check_val("TIOCSWINSZ returns 0", r, 0);

    struct winsize got = {0};
    r = sc3(SYS_IOCTL, 0, TIOCGWINSZ, (long)&got);
    check_val("TIOCGWINSZ after SET returns 0", r, 0);
    check_val("ws_row roundtrip", (long)got.ws_row, 50);
    check_val("ws_col roundtrip", (long)got.ws_col, 132);

    /* TIOCGWINSZ on a pipe must return -ENOTTY (isatty correctness) */
    int pipefd[2];
    r = sc2(SYS_PIPE2, (long)pipefd, 0);
    check_val("pipe2 ok", r, 0);
    struct winsize pw = {0};
    r = sc3(SYS_IOCTL, pipefd[0], TIOCGWINSZ, (long)&pw);
    check_val("TIOCGWINSZ on pipe = -ENOTTY", r, -25);
    sc1(SYS_CLOSE, pipefd[0]);
    sc1(SYS_CLOSE, pipefd[1]);
}

TEST("winsize", test_winsize);
