/*
 * test_gfx.c - host-side check of the drawing primitives in epd_gfx.c.
 *
 *   make -C firmware/hema_epd_clock/test
 *
 * The framebuffer itself is covered end to end by webui/test.mjs, which
 * renders every preset through this same C and diffs it against the JS port
 * byte for byte. That test can only see what reaches the panel, though, and
 * two things here deliberately do not:
 *
 *   - epd_gfx_text_width() of an empty string. Empty text draws nothing
 *     whatever the width says, so a wrong answer is invisible in a rendered
 *     frame - and yet align= subtracts it from the anchor, and a future caller
 *     boxing text would place the box with it.
 *   - epd_gfx_invert() with its corners the wrong way round, which the DSL
 *     cannot produce (INVERT takes a width and a height, both required
 *     positive) but which the function promises to accept.
 *
 * Both are contract, not decoration, so they are pinned where they can be
 * seen rather than left to a renderer that cannot show them.
 */
#include <stdio.h>
#include <string.h>

#include "epd_gfx.h"

static int failures;

static void eq(int got, int want, const char *what)
{
    if (got != want) {
        printf("  FAIL %-46s got %d, want %d\n", what, got, want);
        failures++;
    }
}

/* Count black pixels in the rotated frame, which is what a face can see. */
static int ink(void)
{
    int n = 0;
    for (int16_t y = 0; y < epd_gfx_height(); y++) {
        for (int16_t x = 0; x < epd_gfx_width(); x++) {
            uint32_t idx;
            /* No public getter, so read the packed buffer the way fb_set
             * writes it - via the same rotation the drawing calls used. */
            int16_t px, py;
            switch (epd_gfx_get_rotation()) {
            default:
            case 0: px = x;                  py = y;                   break;
            case 1: px = EPD_WIDTH - 1 - y;  py = x;                    break;
            case 2: px = EPD_WIDTH - 1 - x;  py = EPD_HEIGHT - 1 - y;   break;
            case 3: px = y;                  py = EPD_HEIGHT - 1 - x;   break;
            }
            idx = (uint32_t)py * EPD_WIDTH_BYTES + (px >> 3);
            if (!(epd_framebuffer[idx] & (0x80 >> (px & 7)))) {
                n++;
            }
        }
    }
    return n;
}

int main(void)
{
    printf("epd_gfx:\n");

    /* ---- text metrics ---------------------------------------------------
     * (6n - 1) * scale: 5 px per glyph, a 1 px gap between them, and no gap
     * after the last. webui/test.mjs pins the same numbers for the JS port. */
    eq(epd_gfx_text_width("A", 1), 5, "one glyph at scale 1");
    eq(epd_gfx_text_width("AB", 1), 11, "two glyphs share one gap");
    eq(epd_gfx_text_width("HELLO", 2), 58, "five glyphs at scale 2");
    eq(epd_gfx_text_width("09:41", 5), 145, "an HH:MM face at scale 5");

    /* The case a rendered frame cannot show. (6n - 1) alone would give
     * -scale here, and align= would shift the anchor the wrong way by it. */
    eq(epd_gfx_text_width("", 1), 0, "empty text at scale 1");
    eq(epd_gfx_text_width("", 6), 0, "empty text at scale 6");

    /* scale 0 is treated as 1 rather than collapsing the string to nothing,
     * matching epd_gfx_text() - which clamps it the same way before drawing. */
    eq(epd_gfx_text_width("AB", 0), 11, "scale 0 draws as scale 1");

    /* Clamped rather than allowed to wrap. 30 glyphs at scale 255 is
     * (6*30 - 1) * 255 = 45645, which does not fit in an int16 - truncating it
     * gives -19891, and align=2 would then place the text off to the *right*
     * of the anchor instead of harmlessly off-panel. The string has to be long
     * enough to actually overflow: 20 glyphs at 255 is 30345, which fits, and
     * a test using it passes whether or not the clamp is there. */
    eq(epd_gfx_text_width("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 255), 32767,
       "a width past int16 is clamped, not wrapped");

    /* ---- invert ---------------------------------------------------------
     * Corners in either order, which INVERT() itself cannot produce but the
     * function accepts. Both orders must flip the same 200 pixels. */
    epd_gfx_set_rotation(3);

    epd_gfx_clear(1);
    epd_gfx_invert(10, 10, 29, 19);
    eq(ink(), 200, "invert of a 20x10 box");

    epd_gfx_clear(1);
    epd_gfx_invert(29, 19, 10, 10);
    eq(ink(), 200, "invert with the corners reversed");

    /* Its own inverse. */
    epd_gfx_clear(1);
    epd_gfx_invert(10, 10, 29, 19);
    epd_gfx_invert(10, 10, 29, 19);
    eq(ink(), 0, "inverting twice restores the frame");

    /* Clipped, not wrapped: a box straddling the right edge flips only the
     * part on the panel, and one entirely outside flips nothing. */
    epd_gfx_clear(1);
    epd_gfx_invert(epd_gfx_width() - 5, 0, epd_gfx_width() + 40, 9);
    eq(ink(), 5 * 10, "invert clips at the right edge");

    epd_gfx_clear(1);
    epd_gfx_invert(1000, 1000, 1010, 1010);
    eq(ink(), 0, "invert entirely off-panel draws nothing");

    epd_gfx_clear(1);
    epd_gfx_invert(-40, -40, -1, -1);
    eq(ink(), 0, "invert entirely before the origin draws nothing");

    if (failures) {
        printf("  %d failure(s)\n", failures);
        return 1;
    }
    printf("  all checks passed\n");
    return 0;
}
