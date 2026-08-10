/**
 * epd_cmdparser.c
 */

#include "epd_cmdparser.h"
#include "epd_gfx.h"
#include "epd_time.h"
#include <stdbool.h>
#include <stddef.h>

#define CMD_LINE_MAX   128
#define CMD_TEXT_MAX   64
/* Stored display template.
 *
 * Raising this is NOT one buffer: the same figure sizes four of them, so the
 * cost in RAM is four times the increase.
 *   s_script          here
 *   s_buf, vbuf       epd_store.c, the page-program and read-back copies
 *   restored          user_empty_peripheral_template.c, the boot-time load
 * At 3072 that is 12 KiB of bss, against roughly 41 KiB free between the end
 * of the image and the BLE stack's retention area at 0x07FD4808 - measured
 * from the map, not assumed. Everything slides up together and the heap stays
 * at its configured 1036 bytes, so the arrangement that already works is kept.
 *
 * The figure comes from the case that motivated it. A month grid that aligns
 * each day to its real weekday needs the day's column, and with no way to bind
 * an intermediate value every one of the 31 lines has to repeat the offset
 * expression - which is what costs the bytes, not the grid. Measured at 2099,
 * so 2048 was tried first and missed. The repetition is the better thing to
 * attack (a LET()-style binding would take this back under 1300), but that is
 * a language change and this is a buffer.
 * Note the three redundant copies above are worth removing on their own terms,
 * which would halve the cost of the next increase - but vbuf is the read-back
 * that catches this flash silently failing a write, so that is its own change
 * with its own hardware test, not a rider on a size bump. */
#define CMD_SCRIPT_MAX 3072

/* ---------------------------------------------------------------------------
 * {} template variables
 *
 * Subset of the vendor's variable set (function_doc_official.txt): the
 * date/time ones, which are what a clock face needs. Width suffixes are
 * supported in the vendor's own printf-ish form seen in their downloaded
 * templates, e.g. "{H:02d}" -> "09".
 * ------------------------------------------------------------------------- */

static void append_uint(char *out, uint16_t out_size, uint16_t *n,
                        uint32_t val, uint8_t width, bool zero_pad);

/* Signed wrapper. Every variable was non-negative until {T} arrived, so the
 * unsigned version below was correct by construction and the cast at the call
 * site was invisible - a negative would have rendered as 4294967291 rather
 * than -5. A panel below freezing is a real state (the OTP waveform table runs
 * to -20 C), so it needs the sign.
 *
 * Zero padding puts the sign first, as printf("%03d", -5) does: "-05", not
 * "0-5". The width counts the sign, so {T:03d} is three characters either way.
 * webui/epd.js does the same; the byte-identity test covers a negative. */
static void append_int(char *out, uint16_t out_size, uint16_t *n,
                       int32_t val, uint8_t width, bool zero_pad)
{
    uint32_t mag;

    if (val >= 0) {
        append_uint(out, out_size, n, (uint32_t)val, width, zero_pad);
        return;
    }

    /* Negated as unsigned so INT32_MIN does not overflow on the way. */
    mag = (uint32_t)0 - (uint32_t)val;

    if (zero_pad) {
        if (*n < out_size - 1) out[(*n)++] = '-';
        append_uint(out, out_size, n, mag, width ? width - 1 : 0, true);
    } else {
        char tmp[12];
        uint8_t len = 0;
        uint32_t v = mag;

        do { tmp[len++] = (char)('0' + (v % 10)); v /= 10; }
        while (v && len < sizeof(tmp));
        tmp[len++] = '-';

        while (len < width && len < sizeof(tmp)) tmp[len++] = ' ';
        while (len-- > 0 && *n < out_size - 1) out[(*n)++] = tmp[len];
    }
}

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

/* Language for the text-valued variables {W}, {M} and {P}, chosen by LOCALE().
 *
 * The Chinese and Japanese strings are UTF-8 literals, and every character in
 * them is in tools/glyphs.txt - which is the whole reason that file lists the
 * characters it does. A face selecting a locale whose glyphs are absent would
 * draw a row of blank cells, so the two have to be kept in step; the preview's
 * missing-glyph warning is what catches it. */
#define CMD_LOCALE_EN  0
#define CMD_LOCALE_ZH  1
#define CMD_LOCALE_JA  2
#define CMD_LOCALE_N   3

static uint8_t s_locale;

static const char *const WDAY_NAME[CMD_LOCALE_N][7] = {
    { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" },
    /* 星期日 … 星期六 */
    { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" },
    /* 日曜日 … 土曜日 */
    { "日曜日", "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日" },
};

/* English is three letters, to match WDAY_NAME and because the 5x7 font is the
 * one a Latin face will use. A face that wants "JULY" can spell it out itself.
 *
 * Chinese and Japanese both take the numeric form - 7月, not 七月 - because
 * that is what a printed calendar in either language shows, and because
 * spelling the numerals out would need 十 and the two-character 十一/十二 for
 * no gain a reader would notice. */
static const char *const MONTH_NAME[CMD_LOCALE_N][12] = {
    { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" },
    { "1月", "2月", "3月", "4月", "5月", "6月",
      "7月", "8月", "9月", "10月", "11月", "12月" },
    { "1月", "2月", "3月", "4月", "5月", "6月",
      "7月", "8月", "9月", "10月", "11月", "12月" },
};

/* {P}: 上午/下午 in Chinese, 午前/午後 in Japanese. */
static const char *const AMPM_NAME[CMD_LOCALE_N][2] = {
    { "AM", "PM" },
    { "上午", "下午" },
    { "午前", "午後" },
};

/* Clock snapshot for the script currently being run, taken once in
 * epd_cmd_run(). Reading the clock per reference instead would let a script
 * that straddles a second boundary render {S} inconsistently between its own
 * lines - and would repeat the calendar arithmetic for every variable. */
static epd_tm_t  s_tm;
static uint32_t  s_now;

/* Minutes between repaints, from EVERY() - see the command below. Lives here
 * rather than in the caller because it is part of the script: it is set while
 * the script runs and reset before each run, so it survives a reboot with the
 * stored face and needs no separate persistence. */
#define CMD_EVERY_MAX  1440u          /* a day; longer has no useful meaning */
static uint16_t  s_every_min = 1;

/* Panel temperature for {T}, pushed in by the caller rather than read from the
 * driver directly: this file is compiled on the host against stubs, where
 * epd_ssd1680.c does not exist to link against. Same reason the time arrives
 * through epd_time.c rather than through a GPIO. */
static int8_t    s_temp_c;
static bool      s_temp_valid;

/* Battery for {BAT} and {VCC}, pushed in the same way and for the same reason
 * as the temperature above: reading it needs the SDK's ADC, which does not
 * exist in the host test build. */
static uint8_t   s_batt_pct;
static uint16_t  s_batt_mv;
static bool      s_batt_valid;

/* Exact-match compare, for the multi-letter variable names below. Not strcmp:
 * this file has no string.h and one comparison does not earn it. */
static bool name_is(const char *name, const char *want)
{
    while (*want) {
        if (*name != *want) return false;
        name++; want++;
    }
    return *name == '\0';
}

/* Value of a {} variable as a number. False for the text-valued names ({W},
 * {M}, {P}, {VER}) and for anything unrecognised. Shared by expand_vars()
 * and the expression parser, so a name cannot mean one thing inside FONT text
 * and another in a coordinate. */
static bool var_num(const char *name, int32_t *out)
{
    /* Multi-letter names, matched before the single-letter switch below.
     *
     * An explicit list rather than anything general, because {VER} must NOT
     * be found here: expand_vars() only reaches its text-valued branch when
     * var_num() declines, so a loose match on three-letter names would turn
     * {VER} into a number and lose the version string.
     *
     * Both are gated on a reading having arrived, exactly as {T} is - see the
     * note there. A tag that cannot measure its battery should say so on the
     * panel rather than draw a confident 0%. */
    if (name[0] && name[1]) {
        if (name_is(name, "BAT")) {
            if (!s_batt_valid) { return false; }
            *out = s_batt_pct; return true;
        }
        if (name_is(name, "VCC")) {
            if (!s_batt_valid) { return false; }
            *out = s_batt_mv; return true;
        }
        return false;              /* every other numeric name is one letter */
    }

    if (!name[0]) {
        return false;
    }
    switch (name[0]) {
    case 'y': *out = s_tm.year;  return true;
    case 'm': *out = s_tm.month; return true;
    case 'd': *out = s_tm.day;   return true;
    case 'H': *out = s_tm.hour;  return true;
    case 'N': *out = s_tm.min;   return true;
    case 'S': *out = s_tm.sec;   return true;
    case 'w': *out = s_tm.wday;  return true;
    /* Lower case is the position, upper case the length it runs against:
     * {d}/{D} within the month, {j}/{J} within the year. */
    case 'j': *out = s_tm.yday;  return true;
    case 'J': *out = s_tm.ydays; return true;
    case 'D': *out = s_tm.mdays; return true;
    /* {D} was called {L} before the pairing above existed. Kept, and kept out
     * of the docs, so a face already stored on a tag survives a reflash - an
     * unknown name renders as the literal "{L}" on the panel, which would be a
     * visible break for a tag nobody is holding. */
    case 'L': *out = s_tm.mdays; return true;
    case 'V': *out = s_tm.week;  return true;
    case 'G': *out = s_tm.wyear; return true;
    /* Midnight and noon are 12, not 0. */
    case 'h': *out = (s_tm.hour % 12) ? (s_tm.hour % 12) : 12; return true;
    /* Seconds since 2000. Fits int32 until 2068, which is well past the point
     * at which a CR2032 is the limiting factor. */
    case 'u': *out = (int32_t)s_now; return true;
    /* Panel temperature, whole degrees Celsius, signed. Only answers once
     * something has supplied one - see epd_cmd_set_temp(). Until then it is
     * deliberately NOT a known name, so {T} renders as the literal "{T}" on
     * the panel rather than as a confident 0. A face asking for a temperature
     * on a build that cannot measure one should say so visibly; that is the
     * same reasoning as the {L} note above. */
    case 'T': if (!s_temp_valid) { return false; }
              *out = s_temp_c; return true;
    default:  return false;
    }
}

/* Expand {..} references in `in` into `out`. Unknown names are copied through
 * verbatim (braces included) so a typo is visible on the panel rather than
 * silently vanishing. */
static void expand_vars(const char *in, char *out, uint16_t out_size)
{
    uint16_t n = 0;

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

        int32_t num = 0;

        if (nlen && var_num(name, &num)) {
            append_int(out, out_size, &n, num, width, zero_pad);
        } else if (name[0]=='W' && !name[1]) {
            append_str(out, out_size, &n, WDAY_NAME[s_locale][s_tm.wday % 7]);
        } else if (name[0]=='M' && !name[1]) {
            append_str(out, out_size, &n,
                       MONTH_NAME[s_locale][(s_tm.month - 1) % 12]);
        } else if (name[0]=='P' && !name[1]) {
            append_str(out, out_size, &n,
                       AMPM_NAME[s_locale][s_tm.hour < 12 ? 0 : 1]);
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

/* Leading whitespace is formatting, not a typo: a face is easier to read with
 * its blocks indented, and a DSL that silently drops an indented line is a
 * miserable thing to author against - the tag has no way to say why nothing
 * appeared. Skipped only for *matching*; handle_line() still stores the line
 * as written, so the author's indentation survives a round trip through flash.
 *
 * Note this is the one leniency here. Command names stay case-sensitive, since
 * {d} and {D} already mean different things and a language where the commands
 * fold but the variables do not is worse than one that folds neither. */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Case folding for LOCALE()'s language code, and nothing else.
 *
 * A deliberate exception to the rule just above, not an erosion of it. That
 * rule exists because case already carries meaning in this language - {d} and
 * {D} are different variables - so folding command names would leave it
 * meaningful in one place and not another. A language code has no such
 * distinction to lose: ISO 639-1 defines lowercase, "JA" cannot mean anything
 * else, and the cost of refusing it is a blank face on a shelf label. */
static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* ---------------------------------------------------------------------------
 * Numeric arguments
 *
 * An argument is an expression, not just a literal: integers, {} variables,
 * + - * / %, parentheses and unary minus. This is what makes the variables
 * worth having outside FONT text - without it {d} can be printed but cannot
 * position anything, so a progress bar across the month, a hand that tracks
 * the hour, or a bar chart of the day are all out of reach:
 *
 *     RECT(4,4,4+{d}*8,12,0,1,1)      how far through the month we are
 *     LINE(60,60,60+{H}*2,60,0,2)     a crude hour hand
 *
 * Deliberately simple: no functions, no comparisons, no floats. Everything is
 * int32 and truncating, matching the panel's integer coordinate space.
 *
 * Malformed input yields 0 and keeps going, in line with the rest of the
 * parser - a shelf label should degrade to a wrong-looking face, never to a
 * hang. The two ways that could bite are guarded explicitly: division by zero
 * (undefined in C, and on this core an invisible wrong answer) and unbounded
 * recursion through parentheses on a 1.7 KB stack.
 * ------------------------------------------------------------------------ */

#define EXPR_MAX_DEPTH  8

static int32_t parse_expr(const char **pp, uint8_t depth);

/* number | {var} | '(' expr ')' | ('-'|'+') factor */
static int32_t parse_factor(const char **pp, uint8_t depth)
{
    const char *p = *pp;
    int32_t val = 0;

    while (*p == ' ') p++;

    if (*p == '-') {
        *pp = p + 1;
        return -parse_factor(pp, depth);
    }
    if (*p == '+') {
        *pp = p + 1;
        return parse_factor(pp, depth);
    }

    if (*p == '(') {
        *pp = p + 1;
        /* Past the cap the sub-expression evaluates to 0 rather than
         * recursing: deep nesting is a malformed script, and overflowing the
         * stack here would take out the framebuffer sitting next to it. */
        val = (depth < EXPR_MAX_DEPTH) ? parse_expr(pp, (uint8_t)(depth + 1)) : 0;
        p = *pp;
        while (*p == ' ') p++;
        if (*p == ')') p++;
        *pp = p;
        return val;
    }

    if (*p == '{') {
        char name[12];
        uint8_t n = 0;
        const char *q = p + 1;

        while (*q && *q != '}' && n < sizeof(name) - 1) {
            name[n++] = *q++;
        }
        name[n] = '\0';

        if (*q == '}') {
            if (!var_num(name, &val)) {
                val = 0;           /* text-valued or unknown */
            }
            *pp = q + 1;
            return val;
        }
        /* Unterminated: step over the brace so the caller cannot spin. */
        *pp = p + 1;
        return 0;
    }

    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return val;
}

static int32_t parse_term(const char **pp, uint8_t depth)
{
    int32_t val = parse_factor(pp, depth);

    for (;;) {
        const char *p = *pp;
        char op;

        while (*p == ' ') p++;
        op = *p;
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        *pp = p + 1;

        int32_t rhs = parse_factor(pp, depth);
        if (op == '*') {
            val *= rhs;
        } else if (rhs == 0) {
            val = 0;               /* not a trap, not a wrong answer */
        } else if (op == '/') {
            val /= rhs;
        } else {
            val %= rhs;
        }
    }
    return val;
}

static int32_t parse_expr(const char **pp, uint8_t depth)
{
    int32_t val = parse_term(pp, depth);

    for (;;) {
        const char *p = *pp;
        char op;

        while (*p == ' ') p++;
        op = *p;
        if (op != '+' && op != '-') {
            break;
        }
        *pp = p + 1;

        int32_t rhs = parse_term(pp, depth);
        val = (op == '+') ? (val + rhs) : (val - rhs);
    }
    return val;
}

/* ---------------------------------------------------------------------------
 * Argument lists
 *
 *     COMMAND(pos, pos, ..., name=value, ...)
 *
 * The geometry a command cannot do without stays positional, because that is
 * how it reads: RECT(4,4,60,20). Everything else - colour, stroke width, fill,
 * text scale - is written by name and may be left out.
 *
 * This replaces a purely positional list inherited from the vendor's function
 * reference, which had three problems. Two slots per command were parsed and
 * thrown away because their documentation listed them. Omitting any argument
 * slid every later one into the wrong slot, and since a malformed argument
 * evaluates to 0 rather than complaining, the result was a face that drew in
 * the wrong place with nothing to say why. And no option could ever be added
 * without renumbering the ones after it.
 *
 * Named arguments fix all three, and the third is the one that matters: an
 * option added later cannot disturb a face already stored on a tag.
 *
 * A name is unambiguous because no expression can begin with a letter -
 * parse_factor() consumes digits, '{', '(', '+' and '-' and nothing else - so
 * an argument starting with an identifier followed by '=' is always a named
 * one, never the start of an expression.
 *
 * Options are looked up by re-scanning the argument text per option rather
 * than being collected into a struct first. A line is at most CMD_LINE_MAX and
 * a command has at most a handful of options, so the scanning is free; a
 * struct big enough to hold them would not be, on a 0x700 stack with the
 * framebuffer next to it.
 * ------------------------------------------------------------------------- */

static bool is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* True if the cursor is looking at `name=`, i.e. the positional arguments have
 * run out. Positional reads stop here rather than consuming the option, so a
 * command whose caller omitted a positional still finds its options by name. */
static bool at_named(const char *p)
{
    p = skip_ws(p);
    if (!is_name_start(*p)) {
        return false;
    }
    while (is_name_char(*p)) p++;
    p = skip_ws(p);
    return *p == '=';
}

/* True if `name` (NUL-terminated) is exactly the `len` chars at `s`. The length
 * check on both sides is what stops "color" from matching "colors=1". */
static bool name_eq(const char *s, uint8_t len, const char *name)
{
    for (uint8_t i = 0; i < len; i++) {
        if (name[i] == '\0' || s[i] != name[i]) return false;
    }
    return name[len] == '\0';
}

/* Walk the argument list one top-level argument at a time.
 *
 * On entry *pp is at the start of an argument; on return it is at the start of
 * the next one. If this argument is `name=`, *name points at the name and
 * *name_len is its length, otherwise *name_len is 0. Returns false at the end
 * of the list.
 *
 * Quoted strings and parenthesised sub-expressions are stepped over as units,
 * so neither "FONT(...,'a,scale=3')" nor a comma inside parentheses can be
 * mistaken for an argument boundary. Both looking an option up and checking
 * for unknown ones go through here, so the two cannot disagree about where an
 * argument begins - which they would, being the same fiddly scan written
 * twice. */
static bool next_arg(const char **pp, const char **name, uint8_t *name_len)
{
    const char *p = skip_ws(*pp);
    bool in_str = false;
    uint8_t depth = 0;

    if (*p == '\0' || *p == ')') {
        return false;
    }

    *name = p;
    *name_len = 0;
    if (is_name_start(*p)) {
        const char *q = p;
        while (is_name_char(*q)) q++;
        if (*skip_ws(q) == '=') {
            *name_len = (uint8_t)(q - p);
        }
    }

    while (*p) {
        if (in_str) {
            if (*p == '\'') in_str = false;
            p++;
            continue;
        }
        if (*p == '\'') { in_str = true; p++; continue; }
        if (*p == '(') { depth++; p++; continue; }
        if (*p == ')') {
            if (depth == 0) break;           /* end of the argument list */
            depth--; p++; continue;
        }
        if (*p == ',' && depth == 0) { p++; break; }
        p++;
    }
    *pp = p;
    return true;
}

/* Value of `name=` in an argument list, or NULL if absent. */
static const char *find_named(const char *args, const char *name)
{
    const char *p = args, *nm;
    uint8_t nlen;

    while (next_arg(&p, &nm, &nlen)) {
        if (nlen && name_eq(nm, nlen, name)) {
            const char *v = skip_ws(nm + nlen);
            if (*v == '=') return v + 1;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Problems found while rendering - see epd_cmd_status() in the header.
 * ------------------------------------------------------------------------- */

static uint8_t  s_err_code;     /* epd_err_t of the first problem   */
static uint16_t s_err_line;     /* 1-based, 0 if not line-specific  */
static uint8_t  s_err_count;    /* saturating                       */
static uint16_t s_cur_line;     /* line dispatch_line() is on       */

static void note_err(epd_err_t code)
{
    if (s_err_count == 0) {
        s_err_code = (uint8_t)code;
        s_err_line = s_cur_line;
    }
    if (s_err_count < 255) {
        s_err_count++;
    }
}

/* Options each command reads. Anything else in its argument list is reported
 * rather than silently dropped - a misspelt option looks exactly like one that
 * had no visible effect, which is the hardest kind of mistake to find by
 * staring at the panel.
 *
 * webui/epd.js carries the same table, and a test compares the two: they are
 * the sort of pair that drifts the moment an option is added on one side. */
static const char *const OPTS_NONE[]  = { NULL };
static const char *const OPTS_POINT[] = { "color", NULL };
static const char *const OPTS_LINE[]  = { "color", "width", NULL };
static const char *const OPTS_SHAPE[] = { "color", "width", "fill", NULL };
static const char *const OPTS_TEXT[]  = { "color", "bg", "scale", "align",
                                          "font", NULL };

static void check_options(const char *args, const char *const *known)
{
    const char *p = args, *nm;
    uint8_t nlen;

    while (next_arg(&p, &nm, &nlen)) {
        if (nlen == 0) {
            continue;                        /* positional */
        }
        bool ok = false;
        for (uint8_t i = 0; known[i] != NULL; i++) {
            if (name_eq(nm, nlen, known[i])) { ok = true; break; }
        }
        if (!ok) {
            note_err(EPD_ERR_UNKNOWN_OPT);
        }
    }
}

/* An option's value as a number, or `dflt` if it was not given. The value is a
 * full expression, so `scale=1+{d}%2` is as valid as `scale=2`. */
static int32_t named_int(const char *args, const char *name, int32_t dflt)
{
    const char *v = find_named(args, name);

    if (v == NULL) {
        return dflt;
    }
    return parse_expr(&v, 0);
}

/* Evaluates one positional argument starting at *pp, advances *pp past it and
 * past a following ',' or ')' if present. An empty argument is 0, as before. */
static int32_t parse_int(const char **pp)
{
    if (at_named(*pp)) {
        return 0;                  /* an option - not ours to consume */
    }

    int32_t val = parse_expr(pp, 0);
    const char *p = *pp;

    while (*p == ' ') p++;
    if (*p == ',' || *p == ')') p++;

    *pp = p;
    return val;
}

/* Parses a single-quoted string argument (FONT's text).
 * Writes up to out_size-1 bytes into out, NUL-terminated. */
static void parse_string(const char **pp, char *out, uint16_t out_size)
{
    const char *p = *pp;
    uint16_t n = 0;

    if (at_named(p)) {
        out[0] = '\0';             /* text omitted; leave the option alone */
        return;
    }

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

    line = skip_ws(line);

    /* A line of nothing but spaces is a blank line, not a mistyped command.
     * The run loop already skips truly empty ones, but indentation survives
     * into the stored script, so an editor that leaves trailing whitespace on
     * a separating line produced a spurious "unknown command" against it. */
    if (!line[0]) {
        return;
    }

    if (starts_with(line, "CLEAR(")) {
        const char *args = line + 6;
        p = args;
        int32_t color = parse_int(&p);
        check_options(args, OPTS_NONE);
        epd_gfx_clear((uint8_t)color);
        return;
    }

    /* POINT(x, y, color=)
     * The vendor's $pix and $type are gone rather than kept and ignored: a fat
     * point is LINE(x,y,x,y,width=n), which goes through the same draw_blob(),
     * and $type never meant anything here. */
    if (starts_with(line, "POINT(")) {
        const char *args = line + 6;
        p = args;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        check_options(args, OPTS_POINT);
        int32_t color = named_int(args, "color", 0);
        epd_gfx_set_pixel((int16_t)x, (int16_t)y, (uint8_t)color);
        return;
    }

    /* LINE(x1, y1, x2, y2, color=, width=) */
    if (starts_with(line, "LINE(")) {
        const char *args = line + 5;
        p = args;
        int32_t x1 = parse_int(&p);
        int32_t y1 = parse_int(&p);
        int32_t x2 = parse_int(&p);
        int32_t y2 = parse_int(&p);
        check_options(args, OPTS_LINE);
        int32_t color = named_int(args, "color", 0);
        int32_t width = named_int(args, "width", 1);
        epd_gfx_line((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                     (uint8_t)color, (uint8_t)width);
        return;
    }

    /* RECT(x1, y1, x2, y2, color=, width=, fill=) */
    if (starts_with(line, "RECT(")) {
        const char *args = line + 5;
        p = args;
        int32_t x1 = parse_int(&p);
        int32_t y1 = parse_int(&p);
        int32_t x2 = parse_int(&p);
        int32_t y2 = parse_int(&p);
        int32_t color = named_int(args, "color", 0);
        int32_t width = named_int(args, "width", 1);
        int32_t fill  = named_int(args, "fill", 0);
        check_options(args, OPTS_SHAPE);
        epd_gfx_rect((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2,
                     (uint8_t)color, (uint8_t)width, (uint8_t)fill);
        return;
    }

    /* INVERT(x, y, w, h) - flip every pixel in a box.
     *
     * Width and height, not a second corner, unlike RECT above. RECT inherited
     * x2/y2 from the vendor's command list and is due to change with it; there
     * is no reason to add a second command to the side that is being left. A
     * face highlighting a calendar cell knows the cell's size, not where its
     * far corner lands.
     *
     * Draw it last. The 5x7 font paints its whole glyph cell, so a day number
     * drawn after an inverted box would blank the part of the box it covers. */
    if (starts_with(line, "INVERT(")) {
        const char *args = line + 7;
        p = args;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        int32_t w = parse_int(&p);
        int32_t h = parse_int(&p);
        check_options(args, OPTS_NONE);
        if (w > 0 && h > 0) {          /* an empty box is a no-op, not a fault */
            epd_gfx_invert((int16_t)x, (int16_t)y,
                           (int16_t)(x + w - 1), (int16_t)(y + h - 1));
        }
        return;
    }

    /* CIRCLE(x, y, r, color=, width=, fill=) */
    if (starts_with(line, "CIRCLE(")) {
        const char *args = line + 7;
        p = args;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        int32_t r = parse_int(&p);
        int32_t color = named_int(args, "color", 0);
        int32_t width = named_int(args, "width", 1);
        int32_t fill  = named_int(args, "fill", 0);
        check_options(args, OPTS_SHAPE);
        epd_gfx_circle((int16_t)x, (int16_t)y, (int16_t)r,
                       (uint8_t)color, (uint8_t)width, (uint8_t)fill);
        return;
    }

    /* TEXT(x, y, 'text', color=, bg=, scale=, align=, font=)
     *
     * Called FONT() until this release, which is the vendor's name and was
     * always wrong: it draws a string, and a command called FONT that is not
     * how you choose a font leaves font= looking like a synonym for it. The
     * rename is what frees font= to be the selector. An old face saying FONT()
     * is reported as an unknown command rather than quietly drawing nothing -
     * and a stored one never gets that far, because the store version bump
     * falls it back to the built-in face first.
     * The text is the third positional rather than the eighth: it is the one
     * argument the command is meaningless without, so it belongs with the
     * geometry. The vendor's $g (character spacing, never applied - this font
     * is fixed pitch) and $font_id (there was only ever one font) are gone.
     * bg defaults to 1 because epd_gfx_text() paints the whole glyph cell, so
     * text is opaque; fore=1/bg=0 is how a face draws white on black. */
    if (starts_with(line, "TEXT(")) {
        const char *args = line + 5;
        p = args;
        int32_t x = parse_int(&p);
        int32_t y = parse_int(&p);
        char text[CMD_TEXT_MAX];
        char expanded[CMD_TEXT_MAX];
        parse_string(&p, text, sizeof(text));
        int32_t color = named_int(args, "color", 0);
        int32_t bg    = named_int(args, "bg", 1);
        int32_t scale = named_int(args, "scale", 1);
        int32_t align = named_int(args, "align", 0);
        int32_t font  = named_int(args, "font", EPD_FONT_5X7);
        check_options(args, OPTS_TEXT);
        /* Substitute {H}, {N}, {y}... here rather than at parse time, so a
         * stored script re-rendered on the minute tick picks up the new
         * time (see epd_cmd_run()). */
        expand_vars(text, expanded, sizeof(expanded));

        /* align= moves the anchor, it does not centre within the screen: x is
         * the left edge at 0, the centre at 1, the right edge at 2. Anchoring
         * is the more useful of the two - centring on the panel is just
         * align=1 at x = width/2 - and it is the only one that works for text
         * placed against something other than the frame.
         *
         * Applied after expansion, because the width of "{H:02d}:{N:02d}" is
         * not the width of "09:41". Done here rather than in epd_gfx_text() so
         * the primitive keeps one job; the preview does the same in its FONT
         * case, against the same epd_gfx_text_width() rule. */
        int16_t tx = (int16_t)x;
        if (align != 0) {
            int16_t w = epd_gfx_text_width(expanded, (uint8_t)scale,
                                           (uint8_t)font);
            tx = (int16_t)(tx - ((align == 1) ? w / 2 : w));
        }

        epd_gfx_text(tx, (int16_t)y, expanded,
                     (uint8_t)color, (uint8_t)bg, (uint8_t)scale,
                     (uint8_t)font);
        return;
    }

    /* ROTATE(<0|1|2|3> or <0|90|180|270>) - screen orientation.
     * The vendor's doc defines both forms in the same paragraph ("取值范围为
     * 0、90、180、270" then "1为旋转90度..."), and its example uses degrees,
     * so accept either. 90/270 are landscape; their default is 270. */
    if (starts_with(line, "ROTATE(")) {
        const char *args = line + 7;
        p = args;
        int32_t r = parse_int(&p);
        check_options(args, OPTS_NONE);

        /* Degrees, and only degrees. The vendor's list accepted an index too -
         * so ROTATE(3) meant 270 - and the two spellings overlap at exactly
         * the values a reader is most likely to get wrong: 0 is 0 either way,
         * 1 and 2 and 3 are quarter-turns as an index and very nearly nothing
         * as degrees. A face saying ROTATE(3) and meaning landscape is the
         * single most likely thing to survive from an old script, so it is
         * refused and reported rather than silently taken as 3 degrees and
         * rounded to none.
         *
         * The rotation is left as it was, which for a fresh script is 0 - not
         * "nearest quarter-turn", because guessing here would put the face
         * sideways and give the author nothing to go on. */
        switch (r) {
        case 0:   epd_gfx_set_rotation(0); break;
        case 90:  epd_gfx_set_rotation(1); break;
        case 180: epd_gfx_set_rotation(2); break;
        case 270: epd_gfx_set_rotation(3); break;
        default:  note_err(EPD_ERR_BAD_ARG); break;
        }
        return;
    }

    /* EVERY(n) - repaint every n minutes instead of every minute.
     *
     * A full panel refresh is ~2 s of the most expensive thing this tag does,
     * and the default of once a minute only earns its keep for a face that
     * shows minutes. A calendar redraws 31 identical numbers 1440 times a day
     * to change nothing; EVERY(1440) makes it redraw at midnight, when the
     * date actually turns over.
     *
     * Stored and replayed like any drawing command, so it rides along with the
     * face that wants it and survives a reboot in the same blob - no second
     * characteristic, no second thing for a client to remember to send.
     *
     * Whether a repaint is *due* is the caller's decision, not ours; this only
     * records what the face asked for. Boundaries are absolute rather than
     * measured from the last repaint, so EVERY(60) lands on the hour and
     * EVERY(1440) at midnight instead of drifting to wherever the tag happened
     * to boot. */
    if (starts_with(line, "EVERY(")) {
        const char *args = line + 6;
        p = args;
        int32_t n = parse_int(&p);
        check_options(args, OPTS_NONE);
        if (n < 1) {
            n = 1;                     /* 0 would mean "never repaint" */
        }
        if (n > (int32_t)CMD_EVERY_MAX) {
            n = (int32_t)CMD_EVERY_MAX;
        }
        s_every_min = (uint16_t)n;
        return;
    }

    /* LOCALE(en|zh|ja) - the language {W}, {M} and {P} render in.
     *
     * An ISO 639-1 code rather than an index, for the reason spelled out under
     * ROTATE: a bare 0/1/2 is exactly the sort of opaque number that survives
     * a copy-paste into a face that meant something else by it, and there is
     * nothing in "LOCALE(2)" for a reader to check. Two letters, case
     * insensitive, unquoted - it names a language, not a string to draw.
     *
     * An unknown code is reported and leaves the locale alone, matching
     * ROTATE: guessing a language would put a whole face in the wrong script
     * and give the author nothing to go on. */
    if (starts_with(line, "LOCALE(")) {
        const char *args = line + 7;
        p = skip_ws(args);
        check_options(args, OPTS_NONE);

        char a = lower(p[0]), b = lower(p[1]);
        const char *end = skip_ws(p + 2);

        if (*end != ')') {
            note_err(EPD_ERR_BAD_ARG);       /* not a bare two-letter code */
        } else if (a == 'e' && b == 'n') {
            s_locale = CMD_LOCALE_EN;
        } else if (a == 'z' && b == 'h') {
            s_locale = CMD_LOCALE_ZH;
        } else if (a == 'j' && b == 'a') {
            s_locale = CMD_LOCALE_JA;
        } else {
            note_err(EPD_ERR_BAD_ARG);
        }
        return;
    }

    /* TIME() and RESET() never reach here - they are applied and dropped as
     * they arrive, in handle_line(), so they are neither stored nor replayed. */

    /* Nothing matched. Skipped rather than treated as fatal - a shelf label
     * with no host in range has to keep drawing whatever it can - but counted,
     * so a client can be told which line did nothing. Silence here was the old
     * behaviour and it made a mistyped command indistinguishable from one that
     * simply had no visible effect. */
    note_err(EPD_ERR_UNKNOWN_CMD);
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
static bool     s_script_full;   /* the batch outgrew CMD_SCRIPT_MAX     */
static bool     s_line_long;     /* some line outgrew CMD_LINE_MAX       */
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
    s_line_long = false;
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
/* Centred with align=1 on the middle of the landscape frame, rather than at
 * offsets worked out from the glyph metrics by hand. The old face carried a
 * note like "5 glyphs @5 -> 145 wide" beside each line and an x derived from
 * it, all of which silently became wrong the moment a second font existed.
 *
 * Written out per panel rather than computed from EPD_WIDTH/EPD_HEIGHT. The
 * small panel is not the large one scaled - it is 38 px narrower and 18 px
 * shorter, which changes the spacing between the three lines rather than their
 * sizes, since the fonts do not scale with the frame. Both are mirrored in
 * webui/presets.js as 'Built-in default' and a test diffs them, so the preset
 * and the tag cannot drift apart. */
#if defined(EPD_PANEL_LOW_RES)

/* 212 x 104 landscape, middle x=106. HH:MM at font=1 scale=2 is 168x48, which
 * leaves 56 px for the two 14 px lines under it: 8 above, 8 between, 6 and 6
 * around the pair, 6 below. */
static const char DEFAULT_FACE[] =
    "ROTATE(270)\n"
    "CLEAR(1)\n"
    "TEXT(106,8,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n"
    "TEXT(106,64,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n"
    "TEXT(106,84,'{W}',scale=2,align=1)\n";

#else

/* 250 x 122 landscape, middle x=125. */
static const char DEFAULT_FACE[] =
    "ROTATE(270)\n"
    "CLEAR(1)\n"
    "TEXT(125,18,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n"
    "TEXT(125,78,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n"
    "TEXT(125,100,'{W}',scale=2,align=1)\n";

#endif

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
    /* Matched against the indentation-stripped line, but stored below exactly
     * as it arrived - see skip_ws(). */
    const char *cmd = skip_ws(line);

    if (starts_with(cmd, "TIME(")) {
        const char *p = cmd + 5;
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
    if (starts_with(cmd, "RESET(")) {
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
        s_line_long = false;
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
             * one, rather than dropping the batch.
             *
             * Flagged separately from s_script_full. Both used to set that one
             * flag, so a line over CMD_LINE_MAX was reported to a client as
             * "the script buffer is full" - which sends them shortening the
             * wrong thing, the whole face instead of the one line. */
            s_line_long = true;
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

uint16_t epd_cmd_every_min(void)
{
    return s_every_min;
}

void epd_cmd_set_temp(int8_t c)
{
    s_temp_c = c;
    s_temp_valid = true;
}

void epd_cmd_set_batt(uint8_t pct, uint16_t mv)
{
    s_batt_pct = (pct > 100) ? 100 : pct;
    s_batt_mv = mv;
    s_batt_valid = true;
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

bool epd_cmd_line_pending(void)
{
    return s_line_len != 0u;
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

    /* One clock reading for the whole script, so every {} reference in it -
     * in text and in coordinates alike - describes the same instant. */
    s_now = epd_time_now();
    epd_time_get(&s_tm);

    /* A client's last line often arrives without a trailing newline; commit it
     * before replaying, or it would be silently dropped. */
    flush_partial_line();

    /* The report describes *this* run, so it starts empty every time. The
     * script is replayed each minute, so it is rebuilt identically until a
     * client pushes something new. */
    s_err_code = EPD_ERR_NONE;
    s_err_line = 0;
    s_err_count = 0;

    /* Likewise the refresh interval: reset here so it is a property of the
     * script and nothing else. Were it left standing, dropping EVERY() from a
     * face would silently keep the previous one's interval, and a tag could
     * end up refreshing hourly with no line anywhere saying so. */
    s_every_min = 1;

    /* And the locale, for the same reason: dropping LOCALE() from a face must
     * not leave it rendering in the previous face's language, which is the
     * kind of thing nobody would think to look for. */
    s_locale = CMD_LOCALE_EN;

    if (s_script_full) {
        s_cur_line = 0;                     /* the batch, not one line */
        note_err(EPD_ERR_SCRIPT_FULL);
    }
    if (s_line_long) {
        s_cur_line = 0;                     /* split during the feed, so the
                                               line number is already lost */
        note_err(EPD_ERR_LINE_TOO_LONG);
    }

    uint16_t line_no = 1;
    bool prev_cr = false;

    for (uint16_t i = 0; i < s_script_len; i++) {
        char c = s_script[i];

        if (c == '\n' || c == '\r') {
            /* CRLF ends one line, not two, so the numbers handed to a client
             * match the ones its editor puts in the margin. Rendering is
             * unaffected either way - the empty line between them drew
             * nothing - but a report that is off by one per line is worse
             * than no report. */
            if (c == '\n' && prev_cr) {
                prev_cr = false;
                continue;
            }
            prev_cr = (c == '\r');

            if (overflow) {
                s_cur_line = line_no;
                note_err(EPD_ERR_LINE_TOO_LONG);
            } else if (line_len > 0) {
                line[line_len] = '\0';
                s_cur_line = line_no;
                dispatch_line(line);
            }
            line_len = 0;
            overflow = false;
            line_no++;
            continue;
        }

        prev_cr = false;
        if (line_len < CMD_LINE_MAX - 1) {
            line[line_len++] = c;
        } else {
            /* Drop the whole over-long line rather than silently truncating
             * it into a different, valid-looking command. */
            overflow = true;
        }
    }

    /* Final line without a trailing newline: execute it. Safe here (unlike
     * while streaming) because the script is complete by the time we run. */
    if (overflow) {
        s_cur_line = line_no;
        note_err(EPD_ERR_LINE_TOO_LONG);
    } else if (line_len > 0) {
        line[line_len] = '\0';
        s_cur_line = line_no;
        dispatch_line(line);
    }
}

void epd_cmd_status(uint8_t out[EPD_STATUS_LEN])
{
    out[0] = 2;                                     /* report format version */
    out[1] = s_err_code;
    out[2] = (uint8_t)(s_err_line & 0xFF);
    out[3] = (uint8_t)(s_err_line >> 8);
    out[4] = s_err_count;
    out[5] = (uint8_t)((s_script_full ? 0x01 : 0x00) |
                       (s_line_long   ? 0x02 : 0x00));
    out[6] = (uint8_t)(s_script_len & 0xFF);
    out[7] = (uint8_t)(s_script_len >> 8);
    /* What EVERY() asked for, after clamping. Without this the interval is
     * invisible: a client has no way to tell an honoured EVERY(60) from one
     * the tag never parsed, short of watching the panel for an hour. */
    out[8] = (uint8_t)(s_every_min & 0xFF);
    out[9] = (uint8_t)(s_every_min >> 8);
}
