# Host-side tests for the parts of the firmware that are pure C.
#
#   make          build and run
#   make clean
#
# Nothing here needs the SDK, the toolchain or a tag: the modules under test
# are compiled natively against the stubs in stub/. See test_time.c.

CC      ?= cc
SRC      = ../src/epd
CFLAGS   = -std=c99 -Wall -Wextra -Werror -O1 -I stub -I $(SRC)

TESTS    = test_time test_gfx
TOOLS    = render

.PHONY: all clean render
all: $(addprefix run-,$(TESTS))

run-test_time: test_time
	./$<

test_time: test_time.c $(SRC)/epd_time.c $(SRC)/epd_time.h
	$(CC) $(CFLAGS) -o $@ test_time.c $(SRC)/epd_time.c

run-test_gfx: test_gfx
	./$<

test_gfx: test_gfx.c $(SRC)/epd_gfx.c $(SRC)/epd_gfx.h
	$(CC) $(CFLAGS) -o $@ test_gfx.c $(SRC)/epd_gfx.c

# Not part of `all`: it is a diagnostic tool, not a test. See render.c.
render: render.c $(SRC)/epd_cmdparser.c $(SRC)/epd_gfx.c $(SRC)/epd_time.c
	$(CC) $(CFLAGS) -o $@ render.c \
		$(SRC)/epd_cmdparser.c $(SRC)/epd_gfx.c $(SRC)/epd_time.c

clean:
	rm -f $(TESTS) $(TOOLS)
