/**
 * epd_gfx.c - framebuffer + drawing primitives + minimal fallback font.
 */

#include "epd_gfx.h"
#include <string.h>

uint8_t epd_framebuffer[EPD_BUF_SIZE];

static inline void fb_set(int16_t x, int16_t y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (uint32_t)y * EPD_WIDTH_BYTES + (x >> 3);
    uint8_t mask = 0x80 >> (x & 7);
    if (color) epd_framebuffer[idx] |= mask;   /* 1 = white */
    else       epd_framebuffer[idx] &= ~mask;  /* 0 = black */
}

void epd_gfx_clear(uint8_t color)
{
    memset(epd_framebuffer, color ? 0xFF : 0x00, EPD_BUF_SIZE);
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

/* ---------------------------------------------------------------------
 * Minimal 5x7 fallback font.
 * NOT the vendor's own glyph format (see epd_gfx.h). Covers exactly what
 * a clock/calendar display needs: digits, colon, dash, slash, degree mark,
 * space, and uppercase C (for "12C" style temperature text). Extend the
 * table below for more characters as needed.
 * Each glyph is 5 columns x 7 rows, one byte per column, LSB = top row.
 * ------------------------------------------------------------------- */
typedef struct { char c; uint8_t col[5]; } glyph5x7_t;

static const glyph5x7_t FONT_5X7[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'/', {0x60,0x10,0x08,0x04,0x03}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x62,0x51,0x49,0x49,0x46}},
    {'3', {0x22,0x41,0x49,0x49,0x36}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x2F,0x49,0x49,0x49,0x31}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
};
#define FONT_5X7_COUNT (sizeof(FONT_5X7) / sizeof(FONT_5X7[0]))

static const uint8_t *find_glyph(char c)
{
    for (uint32_t i = 0; i < FONT_5X7_COUNT; i++)
        if (FONT_5X7[i].c == c) return FONT_5X7[i].col;
    return NULL; /* unknown char -> blank */
}

void epd_gfx_text(int16_t x, int16_t y, const char *text, uint8_t fore, uint8_t back, uint8_t scale)
{
    if (scale < 1) scale = 1;
    int16_t cursor = x;

    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = find_glyph(*p);
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = glyph ? glyph[col] : 0x00;
            for (uint8_t row = 0; row < 7; row++) {
                uint8_t on = (bits >> row) & 1;
                uint8_t color = on ? fore : back;
                if (scale == 1) {
                    fb_set(cursor + col, y + row, color);
                } else {
                    for (uint8_t sx = 0; sx < scale; sx++)
                        for (uint8_t sy = 0; sy < scale; sy++)
                            fb_set(cursor + col * scale + sx, y + row * scale + sy, color);
                }
            }
        }
        cursor += (5 + 1) * scale; /* glyph width + 1px gap */
    }
}

void epd_gfx_test_pattern(void)
{
    const int16_t W = EPD_WIDTH;
    const int16_t H = EPD_HEIGHT;

    epd_gfx_clear(1);                              /* white background */

    /* 1px border around the whole panel */
    epd_gfx_rect(0, 0, W - 1, H - 1, 0, 1, 0);

    /* two diagonals corner-to-corner */
    epd_gfx_line(0, 0, W - 1, H - 1, 0, 1);
    epd_gfx_line(W - 1, 0, 0, H - 1, 0, 1);

    /* a filled black rectangle and an outline rectangle */
    epd_gfx_rect(8, 8, 40, 30, 0, 1, 1);           /* filled */
    epd_gfx_rect(W - 41, 8, W - 9, 30, 0, 2, 0);   /* outline, 2px */

    /* a circle in the middle */
    epd_gfx_circle(W / 2, H / 2, 18, 0, 1, 0);

    /* text: prove the font path. White-on-black strip near the bottom. */
    epd_gfx_rect(6, H - 24, W - 6, H - 6, 0, 1, 1);
    epd_gfx_text(12, H - 20, "0123456789", 1, 0, 1);
}
