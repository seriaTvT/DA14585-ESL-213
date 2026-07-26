/**
 * epd_cmdparser.c
 */

#include "epd_cmdparser.h"
#include "epd_gfx.h"
#include "epd_time.h"
#include <stdbool.h>

#define CMD_LINE_MAX   128
#define CMD_TEXT_MAX   64
#define CMD_SCRIPT_MAX 1024   /* stored display template; see script buffer */

/* ---------------------------------------------------------------------------
 * {} template variables
 *
 * Subset of the vendor's variable set (function_doc_official.txt): the
 * date/time ones, which are what a clock face needs. Width suffixes are
 * supported in the vendor's own printf-ish form seen in their downloaded
 * templates, e.g. "{H:02d}" -> "09".
 * ------------------------------------------------------------------------- */

static void append_uint(char *out, uint16_t out_size, uint16_t *n,
                        uint32_t val, uint8_t width, bool zero_pad)
{
    char tmp[12];
    uint8_t len = 0;

    do {
        tmp[len++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val && len < sizeof(tmp));

    while (len < width && len < sizeof(tmp)) {
        tmp[len++] = zero_pad ? '0' : ' ';
    }
    while (len-- > 0 && *n < out_size - 1) {
        out[(*n)++] = tmp[len];
    }
}

static void append_str(char *out, uint16_t out_size, uint16_t *n, const char *s)
{
    while (*s && *n < out_size - 1) {
        out[(*n)++] = *s++;
    }
}

static const char *const WDAY_NAME[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

/* Expand {..} references in `in` into `out`. Unknown names are copied through
 * verbatim (braces included) so a typo is visible on the panel rather than
 * silently vanishing. */
static void expand_vars(const char *in, char *out, uint16_t out_size)
{
    epd_tm_t tm;
    uint16_t n = 0;

    epd_time_get(&tm);

    while (*in && n < out_size - 1) {
        if (*in != '{') {
            out[n++] = *in++;
            continue;
        }

        /* Parse {name} or {name:0Wd} */
        const char *p = in + 1;
        char name[12];
        uint8_t nlen = 0;
        while (*p && *p != '}' && *p != ':' && nlen < sizeof(name) - 1) {
            name[nlen++] = *p++;
        }
        name[nlen] = '\0';

        uint8_t width = 0;
        bool zero_pad = false;
        if (*p == ':') {
            p++;
            if (*p == '0') { zero_pad = true; p++; }
            while (*p >= '0' && *p <= '9') {
                width = (uint8_t)(width * 10 + (*p - '0'));
                p++;
            }
            if (*p == 'd') p++;          /* accept and ignore the conversion */
        }

        if (*p != '}') {                  /* malformed - emit literally */
            out[n++] = *in++;
            continue;
        }
        p++;                              /* skip '}' */

        uint32_t num;
        bool is_num = true;

        if      (!nlen)                 { is_num = false; }
        else if (name[0]=='y' && !name[1]) num = tm.year;
        else if (name[0]=='m' && !name[1]) num = tm.month;
        else if (name[0]=='d' && !name[1]) num = tm.day;
        else if (name[0]=='H' && !name[1]) num = tm.hour;
        else if (name[0]=='N' && !name[1]) num = tm.min;
        else if (name[0]=='S' && !name[1]) num = tm.sec;
        else if (name[0]=='w' && !name[1]) num = tm.wday;
        else if (name[0]=='u' && !name[1]) num = epd_time_now();
        else                                is_num = false;

        if (is_num) {
            append_uint(out, out_size, &n, num, width, zero_pad);
        } else if (name[0]=='W' && !name[1]) {
            append_str(out, out_size, &n, WDAY_NAME[tm.wday % 7]);
        } else if (name[0]=='V' && name[1]=='E' && name[2]=='R' && !name[3]) {
            append_str(out, out_size, &n, "HEMA1");
        } else {
            /* Unknown: copy the original token through. */
            const char *q = in;
            while (q < p && n < out_size - 1) out[n++] = *q++;
        }
        in = p;
    }
    out[n] = '\0';
}

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
        char text[CMD_TEXT_MAX];
        char expanded[CMD_TEXT_MAX];
        parse_string(&p, text, sizeof(text));
        (void)g; (void)font_id;
        /* Substitute {H}, {N}, {y}... here rather than at parse time, so a
         * stored script re-rendered on the minute tick picks up the new
         * time (see epd_cmd_run()). */
        expand_vars(text, expanded, sizeof(expanded));
        epd_gfx_text((int16_t)x, (int16_t)y, expanded, (uint8_t)fore, (uint8_t)back, (uint8_t)scale);
        return;
    }

    /* ROTATE(<0|1|2|3> or <0|90|180|270>) - screen orientation.
     * The vendor's doc defines both forms in the same paragraph ("取值范围为
     * 0、90、180、270" then "1为旋转90度..."), and its example uses degrees,
     * so accept either. 90/270 are landscape; their default is 270. */
    if (starts_with(line, "ROTATE(")) {
        p = line + 7;
        int32_t r = parse_int(&p);
        switch (r) {
        case 90:  r = 1; break;
        case 180: r = 2; break;
        case 270: r = 3; break;
        default:  break;              /* already an index, or 0 */
        }
        epd_gfx_set_rotation((uint8_t)r);
        return;
    }

    /* TIME() and RESET() never reach here - they are applied and dropped as
     * they arrive, in handle_line(), so they are neither stored nor replayed. */

    /* Unrecognized / not-yet-implemented command (CAL, CLOCK, IMG, ICON,
     * TABLE, ROTATE, MIRROR, SHOW, INV, LET, SRAND, RANDS, DATE_OFF,
     * TIME_OFF, EPD, ...) - silently ignored rather than treated as fatal,
     * unlike the vendor firmware's parser which appears to fault on
     * unrecognized tokens in some contexts (see PROTOCOL_NOTES.md section 4
     * "Tier A" note) - deliberately more forgiving here. */
}

/* ---------------------------------------------------------------------------
 * Script buffer
 *
 * The received command batch is STORED rather than executed as it streams in,
 * then replayed in full by epd_cmd_run(). That is what makes the tag a clock
 * instead of a remote framebuffer: the minute tick replays the same script,
 * and the {H}/{N} substitutions inside FONT() re-expand to the new time.
 * It also makes the batch idempotent - a script that opens with CLEAR()
 * repaints from a known state every time.
 *
 * Storing (rather than executing) also means a command may be split across
 * any number of BLE writes: an ATT payload is only MTU-3 bytes - just 20 at
 * the default 23-byte MTU, while "RECT(10,10,100,60,0,1,1)" is 25.
 * ------------------------------------------------------------------------- */
static char     s_script[CMD_SCRIPT_MAX];
static uint16_t s_script_len;
static bool     s_script_full;
static bool     s_batch_pending;

/* Partial line carried between feeds: a BLE write is MTU-3 bytes (20 at the
 * default MTU), so lines routinely arrive split across several of them. */
static char     s_line[CMD_LINE_MAX];
static uint16_t s_line_len;

/* Set when a client's drawing content lands, cleared once persisted. */
static bool     s_dirty;

void epd_cmd_reset(void)
{
    s_script_len = 0;
    s_script_full = false;
    s_batch_pending = false;
    s_line_len = 0;
    s_dirty = false;
}

void epd_cmd_begin_batch(void)
{
    s_batch_pending = true;
}

/* Built-in clock face, used when nothing has been pushed over BLE.
 *
 * Written in the DSL rather than drawn in C on purpose: it then goes through
 * exactly the same parse/expand/render path as a client's template, so the
 * default face and a pushed one cannot drift apart, and a client replaces it
 * simply by sending its own (epd_cmd_begin_batch() clears on the first write).
 *
 * Landscape, so the geometry below is against a 250x122 logical panel. Text
 * width for n glyphs is scale*(6n-1) - 5px glyph, 1px gap, no gap after the
 * last - which is where the x offsets come from: (250 - width) / 2.
 *
 * Unset, the clock reads 00:00 on 2000-01-01 (the epoch), matching the stock
 * firmware's cold-boot behaviour; a host re-syncs with TIME() on connect. */
static const char DEFAULT_FACE[] =
    "ROTATE(3)\n"
    "CLEAR(1)\n"
    "FONT(52,25,0,0,0,1,5,'{H:02d}:{N:02d}')\n"   /* 5 glyphs @5 -> 145 wide */
    "FONT(66,72,0,0,0,1,2,'{y}-{m:02d}-{d:02d}')\n"  /* 10 @2 -> 118 */
    "FONT(108,94,0,0,0,1,2,'{W}')\n";             /* 3 @2 -> 34 */

void epd_cmd_load_default(void)
{
    uint16_t n = (uint16_t)(sizeof(DEFAULT_FACE) - 1);   /* drop the NUL */

    if (n > CMD_SCRIPT_MAX - 1) {
        return;                      /* cannot happen; keeps the copy honest */
    }
    for (uint16_t i = 0; i < n; i++) {
        s_script[i] = DEFAULT_FACE[i];
    }
    s_script_len = n;
    s_script_full = false;
    s_batch_pending = false;
}

/* Handle one complete incoming line.
 *
 * TIME() is applied here and deliberately NOT stored. It is a control command,
 * not part of the picture: storing it made a TIME()-only sync - the obvious
 * thing for a host to send - replace the whole template with a script that
 * draws nothing, so the framebuffer was never repainted again and the face
 * froze at the pre-sync minute while the clock itself ran on correctly.
 *
 * Keeping it out of the buffer also means it can no longer be replayed, which
 * is what the old s_first_run gate existed to prevent. */
static void handle_line(const char *line, uint16_t len)
{
    if (starts_with(line, "TIME(")) {
        const char *p = line + 5;
        int32_t secs = parse_int(&p);
        if (secs > 0) {
            epd_time_set((uint32_t)secs);
        }
        return;
    }

    /* RESET() - start a new template, same as connecting does.
     *
     * Without it a client could only replace the template once per connection:
     * epd_cmd_begin_batch() fires on connect, so a second push on the same
     * connection appended to the first and the two faces drew on top of each
     * other (and a few edit-push cycles overflow CMD_SCRIPT_MAX). That is the
     * normal rhythm of an editor, so the marker is what lets one stay
     * connected while iterating.
     *
     * A control command rather than a timing rule on purpose: ending a batch
     * on a gap in the writes would depend on the connection interval, and a
     * slow link would split one template into several. */
    if (starts_with(line, "RESET(")) {
        epd_cmd_begin_batch();
        return;
    }

    /* First drawing content of a new batch replaces the stored template. Doing
     * it here rather than on the batch's first byte is what lets a TIME()-only
     * batch leave the current face untouched. */
    if (s_batch_pending) {
        s_batch_pending = false;
        s_script_len = 0;
        s_script_full = false;
    }
    s_dirty = true;

    for (uint16_t i = 0; i < len; i++) {
        if (s_script_len < CMD_SCRIPT_MAX - 1) {
            s_script[s_script_len++] = line[i];
        } else {
            s_script_full = true;       /* keep what fits; drop the tail */
        }
    }
    if (s_script_len < CMD_SCRIPT_MAX - 1) {
        s_script[s_script_len++] = '\n';
    }
}

/* Commit a trailing line that arrived without its newline. Callers that are
 * about to act on the script must call this first, or a template whose last
 * line is unterminated would lose that line. */
static void flush_partial_line(void)
{
    if (s_line_len == 0) {
        return;
    }
    s_line[s_line_len] = '\0';
    handle_line(s_line, s_line_len);
    s_line_len = 0;
}

void epd_cmd_feed(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)buf[i];

        if (c == '\n') {
            s_line[s_line_len] = '\0';
            handle_line(s_line, s_line_len);
            s_line_len = 0;
        } else if (s_line_len < CMD_LINE_MAX - 1) {
            s_line[s_line_len++] = c;
        } else {
            /* Over-long line: commit what fits and treat the rest as the next
             * one, rather than dropping the batch. */
            s_script_full = true;
            s_line[s_line_len] = '\0';
            handle_line(s_line, s_line_len);
            s_line_len = 0;
        }
    }
}

bool epd_cmd_script_truncated(void)
{
    return s_script_full;
}

uint16_t epd_cmd_script_len(void)
{
    return s_script_len;
}

const char *epd_cmd_script(void)
{
    return s_script;
}

void epd_cmd_load_script(const char *buf, uint16_t len)
{
    if (len > CMD_SCRIPT_MAX - 1) {
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        s_script[i] = buf[i];
    }
    s_script_len = len;
    s_script_full = false;
    s_batch_pending = false;
    s_line_len = 0;
    s_dirty = false;        /* restored from flash - already persisted */
}

bool epd_cmd_take_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}

void epd_cmd_run(void)
{
    char line[CMD_LINE_MAX];
    uint16_t line_len = 0;
    bool overflow = false;

    /* A client's last line often arrives without a trailing newline; commit it
     * before replaying, or it would be silently dropped. */
    flush_partial_line();

    for (uint16_t i = 0; i < s_script_len; i++) {
        char c = s_script[i];

        if (c == '\n' || c == '\r') {
            if (line_len > 0 && !overflow) {
                line[line_len] = '\0';
                dispatch_line(line);
            }
            line_len = 0;
            overflow = false;
        } else if (line_len < CMD_LINE_MAX - 1) {
            line[line_len++] = c;
        } else {
            /* Drop the whole over-long line rather than silently truncating
             * it into a different, valid-looking command. */
            overflow = true;
        }
    }

    /* Final line without a trailing newline: execute it. Safe here (unlike
     * while streaming) because the script is complete by the time we run. */
    if (line_len > 0 && !overflow) {
        line[line_len] = '\0';
        dispatch_line(line);
    }
}
