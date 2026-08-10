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

/* True if two strings draw different pixels in the same font. Stronger than
 * comparing ink(): distinct glyphs collide on pixel counts all the time. */
static int differs(const char *a, const char *b, uint8_t font)
{
    static uint8_t first[EPD_BUF_SIZE];

    epd_gfx_clear(1);
    epd_gfx_text(0, 0, a, 0, 1, 1, font);
    memcpy(first, epd_framebuffer, EPD_BUF_SIZE);
    int inked = ink();

    epd_gfx_clear(1);
    epd_gfx_text(0, 0, b, 0, 1, 1, font);

    /* Both must actually draw something, or "different" would be satisfied by
     * one of them being a missing glyph. */
    return inked > 0 && ink() > 0 &&
           memcmp(first, epd_framebuffer, EPD_BUF_SIZE) != 0;
}

int main(void)
{
    printf("epd_gfx:\n");

    /* ---- text metrics ---------------------------------------------------
     * (6n - 1) * scale: 5 px per glyph, a 1 px gap between them, and no gap
     * after the last. webui/test.mjs pins the same numbers for the JS port. */
    eq(epd_gfx_text_width("A", 1, EPD_FONT_5X7), 5, "one glyph at scale 1");
    eq(epd_gfx_text_width("AB", 1, EPD_FONT_5X7), 11, "two glyphs share one gap");
    eq(epd_gfx_text_width("HELLO", 2, EPD_FONT_5X7), 58, "five glyphs at scale 2");
    eq(epd_gfx_text_width("09:41", 5, EPD_FONT_5X7), 145, "an HH:MM face at scale 5");

    /* The case a rendered frame cannot show. (6n - 1) alone would give
     * -scale here, and align= would shift the anchor the wrong way by it. */
    eq(epd_gfx_text_width("", 1, EPD_FONT_5X7), 0, "empty text at scale 1");
    eq(epd_gfx_text_width("", 6, EPD_FONT_5X7), 0, "empty text at scale 6");

    /* scale 0 is treated as 1 rather than collapsing the string to nothing,
     * matching epd_gfx_text() - which clamps it the same way before drawing. */
    eq(epd_gfx_text_width("AB", 0, EPD_FONT_5X7), 11, "scale 0 draws as scale 1");

    /* Clamped rather than allowed to wrap. 30 glyphs at scale 255 is
     * (6*30 - 1) * 255 = 45645, which does not fit in an int16 - truncating it
     * gives -19891, and align=2 would then place the text off to the *right*
     * of the anchor instead of harmlessly off-panel. The string has to be long
     * enough to actually overflow: 20 glyphs at 255 is 30345, which fits, and
     * a test using it passes whether or not the clamp is there. */
    eq(epd_gfx_text_width("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 255, EPD_FONT_5X7), 32767,
       "a width past int16 is clamped, not wrapped");

    /* ---- the 16x24 font -------------------------------------------------
     * Same rule, a wider cell: ((16 + 1)n - 1) * scale. */
    eq(epd_gfx_text_width("0", 1, EPD_FONT_16X24), 16, "one large glyph");
    eq(epd_gfx_text_width("00", 1, EPD_FONT_16X24), 33, "two share one gap");
    eq(epd_gfx_text_width("09:41", 1, EPD_FONT_16X24), 84, "HH:MM, large");
    eq(epd_gfx_text_width("09:41", 2, EPD_FONT_16X24), 168, "and at scale 2");
    eq(epd_gfx_text_width("", 1, EPD_FONT_16X24), 0, "empty, large");

    /* A large digit must actually be drawn from the large table, not the
     * small one scaled: at scale 1 a 5x7 '8' cannot ink more than 35 px. */
    epd_gfx_set_rotation(3);
    epd_gfx_clear(1);
    epd_gfx_text(0, 0, "8", 0, 1, 1, EPD_FONT_16X24);
    int big8 = ink();
    epd_gfx_clear(1);
    epd_gfx_text(0, 0, "8", 0, 1, 1, EPD_FONT_5X7);
    int small8 = ink();
    eq(big8 > small8 * 3, 1, "the large '8' is drawn at its own size");
    eq(small8 <= 35, 1, "the small '8' fits a 5x7 cell");

    /* Characters the large table lacks are blank, not folded down to 5x7.
     * bg=1 on a white field means "blank" is literally no ink. */
    epd_gfx_clear(1);
    epd_gfx_text(0, 0, "A", 0, 1, 1, EPD_FONT_16X24);
    eq(ink(), 0, "a letter has no large glyph and draws blank");

    /* ...but the same letter is fine in the small font, so the blank above is
     * the table's doing rather than the text never being drawn at all. */
    epd_gfx_clear(1);
    epd_gfx_text(0, 0, "A", 0, 1, 1, EPD_FONT_5X7);
    eq(ink() > 0, 1, "the same letter draws in 5x7");

    /* Every digit and the colon must be present and non-blank - a mistyped
     * table entry would otherwise leave one character silently invisible. */
    for (const char *c = "0123456789:"; *c; c++) {
        char s[2] = { *c, '\0' };
        epd_gfx_clear(1);
        epd_gfx_text(0, 0, s, 0, 1, 1, EPD_FONT_16X24);
        if (ink() == 0) {
            printf("  FAIL large glyph '%c' is blank\n", *c);
            failures++;
        }
    }

    /* ---- UTF-8 and the 16x16 CJK font -----------------------------------
     * Text is bytes on the wire and characters here. These pin the boundary:
     * a three-byte sequence is one glyph, not three. */
    eq(epd_gfx_text_width("\xe5\xb9\xb4", 1, EPD_FONT_CJK16), 16,
       "one CJK glyph is one 16 px cell");
    eq(epd_gfx_text_width("8", 1, EPD_FONT_CJK16), 8,
       "ASCII in the CJK font is half-width");

    /* The reason width is per glyph rather than per font. '8年' is 8 + gap +
     * 16; a fixed cell would give either 33 or 17 and overlap or gap. */
    eq(epd_gfx_text_width("8\xe5\xb9\xb4", 1, EPD_FONT_CJK16), 25,
       "half- and full-width cells mix in one string");
    eq(epd_gfx_text_width("2026\xe5\xb9\xb4", 1, EPD_FONT_CJK16), 52,
       "a full date line measures as drawn");

    /* Malformed input must resynchronise rather than run off the string. A
     * lone continuation byte and a truncated sequence are each one cell, and
     * the test completing at all is the proof the decoder always advances. */
    eq(epd_gfx_text_width("\x80", 1, EPD_FONT_CJK16), 8, "a stray continuation byte");
    eq(epd_gfx_text_width("\xe5\xb9", 1, EPD_FONT_CJK16), 8, "a truncated sequence");

    /* Lowercase used to fold to uppercase because the 5x7 table was uppercase
     * only. Now it has its own glyphs, so the two must differ.
     *
     * Compared as pixels, not as ink counts: two unrelated 5x7 glyphs land on
     * the same pixel total often enough that a count proves nothing. The
     * degree sign and the tilde below are exactly that case, both 6 px. */
    eq(differs("a", "A", EPD_FONT_5X7), 1, "lowercase is its own glyph, not folded");

    /* The degree sign is the one non-ASCII character in the 5x7 font, and it
     * arrives as two UTF-8 bytes. '~' used to stand in for it and is now a
     * real tilde, so the two must not be the same shape. */
    eq(differs("\xc2\xb0", "~", EPD_FONT_5X7), 1, "the degree sign is not the tilde");

    /* Every glyph in every generated table must have ink. A character listed
     * in tools/glyphs.txt but rendered blank - a typo, or a codepoint the
     * font has no design for - would otherwise be invisible until a face
     * used it. The space is the one legitimate blank. */
    for (uint8_t f = 0; f < EPD_FONT_COUNT; f++) {
        const epd_font_t *font = &EPD_FONTS[f];
        for (uint16_t i = 0; i < font->count; i++) {
            uint32_t cp = font->index[i].cp;
            char s[5];
            int n = 0;
            if (cp == ' ') {
                continue;
            }
            if (cp < 0x80) {
                s[n++] = (char)cp;
            } else if (cp < 0x800) {
                s[n++] = (char)(0xC0 | (cp >> 6));
                s[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                s[n++] = (char)(0xE0 | (cp >> 12));
                s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                s[n++] = (char)(0x80 | (cp & 0x3F));
            }
            s[n] = '\0';

            epd_gfx_clear(1);
            epd_gfx_text(0, 0, s, 0, 1, 1, f);
            if (ink() == 0) {
                printf("  FAIL font %u glyph U+%04lX is blank\n",
                       f, (unsigned long)cp);
                failures++;
            }
        }
    }

    /* A codepoint no font carries draws blank but still advances a full cell,
     * so the gap in the line is where the character was. U+4E2D is not in
     * glyphs.txt; if it is added later this becomes a false failure, which is
     * the right way round - it fails loudly rather than silently passing. */
    epd_gfx_clear(1);
    epd_gfx_text(0, 0, "\xe4\xb8\xad", 0, 1, 1, EPD_FONT_CJK16);
    eq(ink(), 0, "an unlisted character draws blank");
    eq(epd_gfx_text_width("\xe4\xb8\xad", 1, EPD_FONT_CJK16), 8,
       "and still advances a cell");

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

    /* ---- epd_gfx_dirty_rows ------------------------------------------------
     * The band a partial refresh is driven from, so a wrong answer either
     * leaves stale pixels on the glass (band too small) or throws away the
     * saving (too large). Row indices are physical - panel gate lines - which
     * the rotation case at the end is what actually pins. */
    {
        static uint8_t a[EPD_BUF_SIZE], b[EPD_BUF_SIZE];
        uint16_t first, last;

        memset(a, 0xFF, sizeof a);
        memset(b, 0xFF, sizeof b);

        first = last = 0xEEEE;
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 0,
           "identical buffers report nothing dirty");
        eq(first == 0xEEEE && last == 0xEEEE, 1,
           "and leave the outputs untouched");

        /* A single changed byte in the middle. */
        b[40 * EPD_WIDTH_BYTES + 2] ^= 0x08;
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "one changed byte is dirty");
        eq(first, 40, "  band starts at that row");
        eq(last, 40, "  and ends there");

        /* Row 0 alone. lo starts past the end and hi starts at 0, so a band of
         * exactly row 0 is the case where a sloppy emptiness test reports
         * "identical" - which would silently skip the refresh. */
        memset(b, 0xFF, sizeof b);
        b[0] ^= 0x01;
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "row 0 alone is dirty");
        eq(first == 0 && last == 0, 1, "  and reports row 0, not nothing");

        /* The last row, the other boundary. */
        memset(b, 0xFF, sizeof b);
        b[EPD_BUF_SIZE - 1] ^= 0x80;
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "the last row is dirty");
        eq(first == EPD_HEIGHT - 1 && last == EPD_HEIGHT - 1, 1,
           "  and is not read past the end");

        /* Two distant rows: one band spanning both, not two bands. A clock face
         * with a changed digit and a changed date is exactly this. */
        memset(b, 0xFF, sizeof b);
        b[10 * EPD_WIDTH_BYTES] ^= 0x01;
        b[90 * EPD_WIDTH_BYTES] ^= 0x01;
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "two distant rows");
        eq(first, 10, "  band covers the union, from the first");
        eq(last, 90, "  to the last");

        /* Everything. */
        memset(b, 0x00, sizeof b);
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "a full-frame change");
        eq(first == 0 && last == EPD_HEIGHT - 1, 1, "  spans every row");

        /* Rotation independence. Under an odd rotation the logical X axis runs
         * down the panel, so a short horizontal line in face coordinates must
         * come back as a band of MANY physical rows. If this returned rotated
         * coordinates the band would be one row and the refresh would show a
         * sliver of the line. */
        epd_gfx_set_rotation(1);
        epd_gfx_clear(1);
        memcpy(a, epd_framebuffer, sizeof a);
        epd_gfx_line(20, 5, 60, 5, 0, 1);         /* 41 px across, logical */
        memcpy(b, epd_framebuffer, sizeof b);
        eq(epd_gfx_dirty_rows(a, b, &first, &last), 1, "rotated line is dirty");
        eq(last - first >= 40, 1,
           "  a rotated horizontal line spans many physical rows");
        epd_gfx_set_rotation(0);
    }

    if (failures) {
        printf("  %d failure(s)\n", failures);
        return 1;
    }
    printf("  all checks passed\n");
    return 0;
}
