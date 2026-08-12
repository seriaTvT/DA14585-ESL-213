/**
 * epd_gfx.c - framebuffer + drawing primitives + minimal fallback font.
 */

#include "epd_gfx.h"
#include <stdbool.h>
#include <string.h>

uint8_t epd_framebuffer[EPD_BUF_SIZE_MAX];

static uint8_t s_rotation;   /* quarter-turns clockwise, 0..3 */

void epd_gfx_set_rotation(uint8_t r)
{
    s_rotation = r & 3;
}

uint8_t epd_gfx_get_rotation(void)
{
    return s_rotation;
}

int16_t epd_gfx_width(void)
{
    return (s_rotation & 1) ? epd_height : epd_width;
}

int16_t epd_gfx_height(void)
{
    return (s_rotation & 1) ? epd_width : epd_height;
}

bool epd_gfx_dirty_rows(const uint8_t *a, const uint8_t *b,
                        uint16_t *first, uint16_t *last)
{
    /* epd_height and epd_width_bytes on purpose, not the rotated accessors -
     * see the header. A framebuffer row is a panel gate line whatever rotation
     * a face asked for. */
    uint16_t lo = epd_height;   /* past the end = nothing found yet */
    uint16_t hi = 0;
    uint16_t row;

    for (row = 0; row < epd_height; row++) {
        size_t off = (size_t)row * epd_width_bytes;

        if (memcmp(a + off, b + off, epd_width_bytes) != 0) {
            if (lo > row) {
                lo = row;
            }
            hi = row;
        }
    }

    /* Only reachable with lo still past the end, so this is "identical" and not
     * an empty band that a caller might otherwise refresh. */
    if (lo > hi) {
        return false;
    }

    *first = lo;
    *last = hi;
    return true;
}

/* Rotated coordinate -> byte index and bit mask. False if it falls outside the
 * visible area, in which case the outputs are untouched.
 *
 * Shared by every write below rather than repeated per operation: the rotation
 * transform is the one piece of this file that must agree everywhere, and a
 * second hand-copied switch is how a rotated build ends up inverting a
 * different rectangle from the one it fills. */
static inline bool fb_addr(int16_t x, int16_t y, uint32_t *idx, uint8_t *mask)
{
    /* Bounds are checked in the rotated frame, before the transform - so a
     * clipped shape clips against what the caller can actually see. */
    if (x < 0 || y < 0 || x >= epd_gfx_width() || y >= epd_gfx_height()) {
        return false;
    }

    int16_t px, py;
    switch (s_rotation) {
    default:
    case 0: px = x;                  py = y;                   break;
    case 1: px = epd_width - 1 - y;  py = x;                    break;
    case 2: px = epd_width - 1 - x;  py = epd_height - 1 - y;   break;
    case 3: px = y;                  py = epd_height - 1 - x;   break;
    }

    *idx  = (uint32_t)py * epd_width_bytes + (px >> 3);
    *mask = 0x80 >> (px & 7);
    return true;
}

static inline void fb_set(int16_t x, int16_t y, uint8_t color)
{
    uint32_t idx;
    uint8_t mask;
    if (!fb_addr(x, y, &idx, &mask)) return;

    if (color) epd_framebuffer[idx] |= mask;   /* 1 = white */
    else       epd_framebuffer[idx] &= ~mask;  /* 0 = black */
}

static inline void fb_xor(int16_t x, int16_t y)
{
    uint32_t idx;
    uint8_t mask;
    if (!fb_addr(x, y, &idx, &mask)) return;

    epd_framebuffer[idx] ^= mask;
}

void epd_gfx_clear(uint8_t color)
{
    memset(epd_framebuffer, color ? 0xFF : 0x00, epd_buf_size);
}

void epd_gfx_set_pixel(int16_t x, int16_t y, uint8_t color)
{
    fb_set(x, y, color);
}

static void draw_blob(int16_t x, int16_t y, uint8_t color, uint8_t pix)
{
    int16_t half = pix / 2;
    for (int16_t dy = 0; dy < pix; dy++)
        for (int16_t dx = 0; dx < pix; dx++)
            fb_set(x - half + dx, y - half + dy, color);
}

void epd_gfx_line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix)
{
    /* Bresenham */
    int16_t dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int16_t sx = (x1 < x2) ? 1 : -1;
    int16_t dy = (y2 > y1) ? (y1 - y2) : (y2 - y1); /* negative magnitude */
    int16_t sy = (y1 < y2) ? 1 : -1;
    int16_t err = dx + dy;

    if (pix < 1) pix = 1;

    for (;;) {
        draw_blob(x1, y1, color, pix);
        if (x1 == x2 && y1 == y2) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void epd_gfx_rect(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix, uint8_t filled)
{
    if (filled) {
        for (int16_t y = y1; y <= y2; y++)
            epd_gfx_line(x1, y, x2, y, color, 1);
    } else {
        epd_gfx_line(x1, y1, x2, y1, color, pix);
        epd_gfx_line(x1, y2, x2, y2, color, pix);
        epd_gfx_line(x1, y1, x1, y2, color, pix);
        epd_gfx_line(x2, y1, x2, y2, color, pix);
    }
}

void epd_gfx_circle(int16_t x0, int16_t y0, int16_t r, uint8_t color, uint8_t pix, uint8_t filled)
{
    int16_t x = r, y = 0, err = 0;
    while (x >= y) {
        if (filled) {
            epd_gfx_line(x0 - x, y0 + y, x0 + x, y0 + y, color, 1);
            epd_gfx_line(x0 - x, y0 - y, x0 + x, y0 - y, color, 1);
            epd_gfx_line(x0 - y, y0 + x, x0 + y, y0 + x, color, 1);
            epd_gfx_line(x0 - y, y0 - x, x0 + y, y0 - x, color, 1);
        } else {
            draw_blob(x0 + x, y0 + y, color, pix);
            draw_blob(x0 + y, y0 + x, color, pix);
            draw_blob(x0 - y, y0 + x, color, pix);
            draw_blob(x0 - x, y0 + y, color, pix);
            draw_blob(x0 - x, y0 - y, color, pix);
            draw_blob(x0 - y, y0 - x, color, pix);
            draw_blob(x0 + y, y0 - x, color, pix);
            draw_blob(x0 + x, y0 - y, color, pix);
        }
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
}

void epd_gfx_invert(int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    /* Accept the corners in either order. A face computing a cell from an
     * expression can easily produce them the other way round, and silently
     * drawing nothing would be the least useful response. */
    if (x1 > x2) { int16_t t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int16_t t = y1; y1 = y2; y2 = t; }

    /* Per pixel, through the same clip and transform as every other write.
     * Byte-at-a-time would be faster, but only in the unrotated case - under
     * rotation 1 and 3 a framebuffer byte is 8 pixels along the *panel's* x
     * axis, which runs down the screen, so there is no run of 8 to batch. Not
     * worth two code paths for something that runs once a repaint. */
    for (int16_t y = y1; y <= y2; y++) {
        for (int16_t x = x1; x <= x2; x++) {
            fb_xor(x, y);
        }
    }
}

/* Next codepoint from a UTF-8 string, advancing *p past it.
 *
 * Text arrives as bytes and has done since the DSL was ASCII-only, so this is
 * where a byte string becomes characters. Malformed input yields U+FFFD, which
 * no font carries, so it draws as one blank cell.
 *
 * A bad sequence consumes its maximal valid subpart and no more - the byte
 * that ended it is re-examined, because a truncated character followed by a
 * good one should cost one blank cell rather than swallowing its neighbour.
 * Every path advances *p by at least one byte, which is what stops a face
 * sending us rubbish from becoming an endless loop.
 *
 * Reading s[i] cannot overrun a NUL-terminated string: a truncated sequence
 * ends at either the NUL or a non-continuation byte, and both fail the 0xC0
 * test before the index moves past them. */
static uint32_t utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    uint8_t  n;

    if (c < 0x80)                { *p += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07u; }
    else                         { *p += 1; return 0xFFFDu; }

    for (uint8_t i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFDu; }
        c = (c << 6) | (uint32_t)(s[i] & 0x3Fu);
    }
    *p += n + 1;
    return c;
}

/* Glyph for a codepoint, or NULL. The index is sorted, so this bisects: the
 * CJK font is 126 entries and a face redraws every glyph on every repaint. */
static const epd_glyph_t *font_find(const epd_font_t *f, uint32_t cp)
{
    if (cp > 0xFFFFu) {
        return NULL;                  /* index keys are 16-bit; astral is out */
    }

    uint16_t lo = 0, hi = f->count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        uint16_t at  = f->index[mid].cp;
        if      (at < cp) lo = (uint16_t)(mid + 1);
        else if (at > cp) hi = mid;
        else              return &f->index[mid];
    }
    return NULL;
}

/* Cell width of one codepoint, excluding the 1 px gap that follows it.
 *
 * Per glyph rather than per font, because the 16x16 face stores ASCII at 8 px:
 * '2026年' has to advance 8 px four times and then 16, or the year and the
 * character after it overlap. A missing glyph still advances a full cell, so
 * the gap in the line is where the character was. */
static uint8_t glyph_w(const epd_font_t *f, uint32_t cp)
{
    const epd_glyph_t *g = font_find(f, cp);
    return g ? g->w : f->index[0].w;
}

int16_t epd_gfx_text_width(const char *text, uint8_t scale, uint8_t font)
{
    if (scale < 1) scale = 1;
    if (font >= EPD_FONT_COUNT) font = EPD_FONT_5X7;

    const epd_font_t *f = &EPD_FONTS[font];
    int32_t w = 0;

    /* Each glyph is its cell width plus a 1 px gap; the last gap is not drawn,
     * so this sums (w + 1) and subtracts the one trailing gap at the end.
     * Computed in 32 bits and clamped: a face is free to ask for scale=200,
     * and the wrapped negative that would produce is exactly the kind of thing
     * that draws in the wrong place instead of simply off-panel. */
    for (const char *p = text; *p; ) {
        w += (int32_t)glyph_w(f, utf8_next(&p)) + 1;
    }
    if (w == 0) {
        return 0;              /* not -scale: there is no trailing gap to trim */
    }

    w = (w - 1) * (int32_t)scale;
    return (w > 32767) ? (int16_t)32767 : (int16_t)w;
}

void epd_gfx_text(int16_t x, int16_t y, const char *text, uint8_t fore,
                  uint8_t back, uint8_t scale, uint8_t font)
{
    if (scale < 1) scale = 1;
    if (font >= EPD_FONT_COUNT) font = EPD_FONT_5X7;

    const epd_font_t *f  = &EPD_FONTS[font];
    const uint8_t     gh = f->h;
    int16_t cursor = x;

    for (const char *p = text; *p; ) {
        const epd_glyph_t *g  = font_find(f, utf8_next(&p));
        const uint8_t      gw = g ? g->w : f->index[0].w;

        for (uint8_t col = 0; col < gw; col++) {
            for (uint8_t row = 0; row < gh; row++) {
                /* Column-major with the LSB at the top row, bpc bytes per
                 * column: byte (row / 8), bit (row % 8). For the 5x7 font
                 * bpc is 1 and row < 8 always, so this reduces to the
                 * single-byte form it had before there were three fonts. */
                uint8_t bits = 0x00;
                if (g) {
                    bits = f->bits[g->off + col * f->bpc + (row >> 3)];
                }
                uint8_t color = ((bits >> (row & 7)) & 1) ? fore : back;

                if (scale == 1) {
                    fb_set(cursor + col, y + row, color);
                } else {
                    for (uint8_t sx = 0; sx < scale; sx++)
                        for (uint8_t sy = 0; sy < scale; sy++)
                            fb_set(cursor + col * scale + sx, y + row * scale + sy, color);
                }
            }
        }
        cursor += (int16_t)((gw + 1) * scale);   /* glyph width + 1px gap */
    }
}
