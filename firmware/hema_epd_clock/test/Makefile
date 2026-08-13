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

TESTS    = test_time test_gfx test_board
TOOLS    = render

.PHONY: all clean render
# `all` runs the tests and BUILDS the tools without running them. The tools are
# diagnostics rather than tests, so running them here would mean nothing - but
# leaving them out of the default target entirely is how render.c came to spend
# weeks not compiling at all, unnoticed, while a stale binary from before the
# rename sat beside it still working. Building is the cheap half of the check.
all: $(addprefix run-,$(TESTS)) $(TOOLS)

run-test_time: test_time
	./$<

test_time: test_time.c $(SRC)/epd_time.c $(SRC)/epd_time.h
	$(CC) $(CFLAGS) -o $@ test_time.c $(SRC)/epd_time.c

run-test_gfx: test_gfx
	./$<

test_gfx: test_gfx.c $(SRC)/epd_gfx.c $(SRC)/epd_gfx.h $(SRC)/epd_font_data.c
	$(CC) $(CFLAGS) -o $@ test_gfx.c $(SRC)/epd_gfx.c $(SRC)/epd_font_data.c

run-test_board: test_board
	./$<

# Built for the default variant. The decoder itself is variant-agnostic; only
# the build-agreement check at the end reads the macros, and it is written to
# assert the right answer either way, so -DEPD_BOARD_VARIANT_A also passes.
test_board: test_board.c $(SRC)/epd_board.c $(SRC)/epd_board.h
	$(CC) $(CFLAGS) -o $@ test_board.c $(SRC)/epd_board.c

# A diagnostic tool, not a test - built by `all`, never run by it. See render.c.
#
# ONE binary for both panels: pass --panel high|low. There used to be a second,
# `render-low`, built with -DEPD_PANEL_LOW_RES, back when geometry was a
# compile-time constant. It is runtime now, on the tag and here alike.
render: render.c $(SRC)/epd_cmdparser.c $(SRC)/epd_gfx.c $(SRC)/epd_time.c $(SRC)/epd_font_data.c
	$(CC) $(CFLAGS) -o $@ render.c \
		$(SRC)/epd_cmdparser.c $(SRC)/epd_gfx.c $(SRC)/epd_time.c $(SRC)/epd_font_data.c

clean:
	rm -f $(TESTS) $(TOOLS)
