# LTP mini-test suite — static musl binaries for ABI validation
CC      = musl-gcc
CFLAGS  = -static -O2 -Wall -Wextra -Werror -std=c11
TESTS   = ltp_open ltp_readwrite ltp_mmap ltp_fork ltp_exec \
          ltp_pipe ltp_signal ltp_socket ltp_poll ltp_time

all: $(TESTS)

ltp_%: ltp_%.c ltp.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)

.PHONY: all clean
