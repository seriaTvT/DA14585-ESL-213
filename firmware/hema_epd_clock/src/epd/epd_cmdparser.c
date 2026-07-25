/**
 * epd_cmdparser.c
 */

#include "epd_cmdparser.h"
#include "epd_gfx.h"
#include <stdbool.h>

#define CMD_LINE_MAX 128

static bool starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

/* Parses a signed decimal integer starting at *pp, advances *pp past it and
 * past a following ',' or ')' if present. Does NOT implement the vendor's
 * {}-variable / arithmetic-expression syntax - plain integer literals only. */
static int32_t parse_int(const char **pp)
{
    const char *p = *pp;
    bool neg = false;
    int32_t val = 0;

    while (*p == ' ') p++;
    if (*p == '-') { neg = true; p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ') p++;
    if (*p == ',' || *p == ')') p++;

    *pp = p;
    return neg ? -val : val;
}

/* Parses a single-quoted string argument (for FONT's trailing 'text' arg).
 * Writes up to out_size-1 bytes into out, NUL-terminated. */
static void parse_string(const char **pp, char *out, uint16_t out_size)
{
    const char *p = *pp;
    uint16_t n = 0;

    while (*p == ' ') p++;
    if (*p == '\'') {
        p++;
        while (*p && *p != '\'' && n < out_size - 1) {
            out[n++] = *p++;
        }
        if (*p == '\'') p++;
    }
    out[n] = '\0';

    while (*p == ' ') p++;
    if (*p == ',' || *p == ')') p++;
    *pp = p;
}

static void dispatch_line(const char *line)
{
    const char *p;

    if (starts_with(line, "CLEAR(")) {
        p = line + 6;
        int32_t color = parse_int(&p);
        epd_gfx_clear((uint8_t)color);
        return;
    }

    if (starts_with(line, "POINT(")) {
        p = line + 6;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        int32_t color = parse_int(&p);
        int32_t pix = parse_int(&p);
        /* $type (last arg) intentionally ignored - no distinct point
         * "types" implemented yet, matches nothing being documented
         * beyond size for POINT in function_doc_official.txt. */
        (void)pix;
        epd_gfx_set_pixel((int16_t)x, (int16_t)y, (uint8_t)color);
        return;
    }

    if (starts_with(line, "LINE(")) {
        p = line + 5;
        int32_t x1 = parse_int(&p);
        int32_t y1 = parse_int(&p);
        int32_t x2 = parse_int(&p);
        int32_t y2 = parse_int(&p);
        int32_t color = parse_int(&p);
        int32_t pix = parse_int(&p);
        epd_gfx_line((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                     (uint8_t)color, (uint8_t)pix);
        return;
    }

    if (starts_with(line, "RECT(")) {
        p = line + 5;
        int32_t x1 = parse_int(&p);
        int32_t y1 = parse_int(&p);
        int32_t x2 = parse_int(&p);
        int32_t y2 = parse_int(&p);
        int32_t color = parse_int(&p);
        int32_t pix = parse_int(&p);
        int32_t type = parse_int(&p);
        epd_gfx_rect((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                     (uint8_t)color, (uint8_t)pix, (uint8_t)type);
        return;
    }

    if (starts_with(line, "CIRCLE(")) {
        p = line + 7;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        int32_t r = parse_int(&p);
        int32_t color = parse_int(&p);
        int32_t pix = parse_int(&p);
        int32_t type = parse_int(&p);
        epd_gfx_circle((int16_t)x, (int16_t)y, (int16_t)r,
                       (uint8_t)color, (uint8_t)pix, (uint8_t)type);
        return;
    }

    if (starts_with(line, "FONT(")) {
        p = line + 5;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        int32_t g = parse_int(&p);          /* char spacing - not applied by
                                                the fallback font (fixed
                                                pitch), accepted for
                                                compatibility */
        int32_t font_id = parse_int(&p);    /* ignored - only one built-in
                                                fallback font exists so far */
        int32_t fore = parse_int(&p);
        int32_t back = parse_int(&p);
        int32_t scale = parse_int(&p);
        char text[64];
        parse_string(&p, text, sizeof(text));
        (void)g; (void)font_id;
        epd_gfx_text((int16_t)x, (int16_t)y, text, (uint8_t)fore, (uint8_t)back, (uint8_t)scale);
        return;
    }

    /* Unrecognized / not-yet-implemented command (CAL, CLOCK, IMG, ICON,
     * TABLE, ROTATE, MIRROR, SHOW, INV, LET, SRAND, RANDS, DATE_OFF,
     * TIME_OFF, EPD, ...) - silently ignored rather than treated as fatal,
     * unlike the vendor firmware's parser which appears to fault on
     * unrecognized tokens in some contexts (see PROTOCOL_NOTES.md section 4
     * "Tier A" note) - deliberately more forgiving here. */
}

void epd_cmd_process(const uint8_t *buf, uint16_t len)
{
    char line[CMD_LINE_MAX];
    uint16_t line_len = 0;

    for (uint16_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line[line_len] = '\0';
                dispatch_line(line);
                line_len = 0;
            }
        } else if (line_len < CMD_LINE_MAX - 1) {
            line[line_len++] = c;
        }
    }
    /* Trailing command with no terminating newline - still execute it,
     * matching a BLE write that ends exactly at the command's ')'. */
    if (line_len > 0) {
        line[line_len] = '\0';
        dispatch_line(line);
    }
}
