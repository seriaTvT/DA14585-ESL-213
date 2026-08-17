/* Host-side test for the board-record decoder.
 *
 * The fixtures are the real thing: sixteen bytes lifted from flash 0x039000 of
 * each factory dump taken 2026-08-12, and the expected pin maps are the tables
 * those same tags build in their own RAM at 0x07FD4310. So this does not check
 * the decoder against my reading of the format - it checks it against three
 * tags, including the erased case that is easy to get backwards.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I ../src/epd -o test_board \
 *      test_board.c ../src/epd/epd_board.c && ./test_board
 */
#include <stdio.h>
#include <string.h>
#include "epd_board.h"

static int failures;

static void check(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) {
        failures++;
    }
}

/* Expected maps, as (port,pin) pairs in this module's signal order:
 *      CS  AUX  RST  SCK  SDA  DC  BUSY  PWR                                */
static const uint8_t variant_b[] = { 2,1, 1,1, 0,7, 0,0, 0,6, 0,5, 2,0, 2,3 };
static const uint8_t variant_a[] = { 2,1, 2,2, 1,0, 0,1, 2,0, 0,7, 1,1, 2,3 };

static void check_map(const epd_board_t *b, const uint8_t *want, const char *what)
{
    int i, ok = 1;

    for (i = 0; i < EPD_BOARD_NPINS; i++) {
        if (b->port[i] != want[2 * i] || b->pin[i] != want[2 * i + 1]) {
            ok = 0;
        }
    }
    check(ok, what);
    if (!ok) {
        printf("      got:");
        for (i = 0; i < EPD_BOARD_NPINS; i++) {
            printf(" P%u_%u", b->port[i], b->pin[i]);
        }
        printf("\n      want:");
        for (i = 0; i < EPD_BOARD_NPINS; i++) {
            printf(" P%u_%u", want[2 * i], want[2 * i + 1]);
        }
        printf("\n");
    }
}

int main(void)
{
    epd_board_t b;

    /* --- Type 1: A53 panel, record erased -> the built-in variant-B map --- */
    static const uint8_t type1[16] = {
        0x14, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    printf("Type 1 (variant B, A53) - record erased\n");
    check(epd_board_decode(type1, &b) == false, "no pin map in the record");
    check(b.panel == EPD_BOARD_PANEL_A53,       "panel decodes as A53 (0x14)");
    check(b.have_pinmap == false,               "have_pinmap false");
    check_map(&b, variant_b,                    "falls back to the variant-B default");

    /* --- Type 3: A41 panel, selector 0x01, map present -> variant A ------- */
    static const uint8_t type3[16] = {
        0x09, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x21, 0x22, 0x10, 0x01, 0x20, 0x07, 0x11, 0x23
    };
    printf("Type 3 (variant A, A41) - record written\n");
    check(epd_board_decode(type3, &b) == true,  "pin map found");
    check(b.panel == EPD_BOARD_PANEL_A41,       "panel decodes as A41 (0x09)");
    check(b.have_pinmap == true,                "have_pinmap true");
    check_map(&b, variant_a,                    "unpacks to the tag's live table");

    /* --- Type 4: A41 panel, record erased -> variant B on an A41 board ----
     *
     * This is Type 6's record too, byte for byte - a different board with a
     * socketed panel, indistinguishable from flash. Deliberately not a second
     * case: the same sixteen bytes cannot decode two ways, and a case that
     * asserts nothing new is one more thing to keep in step. What the record
     * cannot tell you about Type 6 is in hema-local/re/type6/README.md. */
    static const uint8_t type4[16] = {
        0x09, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    printf("Types 4 and 6 (variant B, A41) - record erased\n");
    check(epd_board_decode(type4, &b) == false, "no pin map in the record");
    check(b.panel == EPD_BOARD_PANEL_A41,       "panel decodes as A41 (0x09)");
    check_map(&b, variant_b,                    "falls back to the variant-B default");

    /* --- A wholly blank sector, which is what a synthesised image leaves -- */
    static const uint8_t blank[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    printf("Blank record\n");
    check(epd_board_decode(blank, &b) == false,   "no pin map");
    check(b.panel == EPD_BOARD_PANEL_UNKNOWN,     "panel unknown, not guessed");
    check_map(&b, variant_b,                      "still yields a usable default");

    /* --- Selector set but the map is nonsense: keep the default whole ----- */
    static const uint8_t bad[16] = {
        0x09, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x21, 0x22, 0x10, 0x01, 0x20, 0x07, 0x11, 0x9f  /* port 9, pin 15 */
    };
    printf("Selector set, map malformed\n");
    check(epd_board_decode(bad, &b) == false,     "rejected");
    check_map(&b, variant_b,                      "default left intact, not half-applied");

    /* Port 1 has only six pins, so P1_7 is not a real pin even though the
     * nibble is in range. This is the check that a bounds test on the nibble
     * alone would let through. */
    static const uint8_t bad_pin[16] = {
        0x09, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x21, 0x22, 0x17, 0x01, 0x20, 0x07, 0x11, 0x23
    };
    printf("Selector set, pin out of range for its port (P1_7)\n");
    check(epd_board_decode(bad_pin, &b) == false, "rejected");

    /* --- The build-agreement check, both ways --------------------------- */
    /* The "does this record agree with the build?" cases lived here and are
     * gone with the function they tested. There is no build-specific target to
     * agree with any more: one image reads its wiring and geometry out of the
     * record, so the record cannot disagree with it.
     *
     * What is still worth asserting is that a blank record decodes to the
     * BUILT-IN case rather than to nothing, because that is what every
     * variant-B tag carries and what the firmware then drives. */
    epd_board_decode(blank, &b);
    check(b.have_pinmap == false, "a blank record claims no explicit map");
    check(b.panel == EPD_BOARD_PANEL_UNKNOWN,
          "a blank record asserts no panel, rather than guessing one");
    check(b.port[EPD_BOARD_CS] == 2 && b.pin[EPD_BOARD_CS] == 1,
          "and still yields the built-in map, CS on P2_1");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
