/*
 * epd.js - a JS port of the firmware's renderer, for the live preview.
 *
 * This is deliberately a *port*, not an approximation: the same Bresenham, the
 * same rotation transform, the same 5x7 glyph table, the same {} expansion as
 * src/epd/epd_gfx.c and src/epd/epd_cmdparser.c. The whole point of the
 * preview is that what you see is what the panel will show, so anything that
 * drifts from the firmware here is a bug in this file - if you change a
 * primitive in the firmware, change it here too.
 *
 * The one intentional difference: the firmware silently ignores commands it
 * doesn't implement (a shelf label should not brick itself on a typo), while
 * this reports them, because at authoring time a silent no-op is the least
 * helpful thing possible.
 */

import { FONTS, EPD_FONT_5X7, EPD_FONT_16X24, EPD_FONT_CJK16 }
  from './font_data.js';

/* Native panel geometry - portrait, as the SSD1680 sees it.
 *
 * Two panels are in the field and they are not interchangeable. Which one a tag
 * has is a property of the tag, and nothing in the BLE protocol says which -
 * so it is the operator's job to pick the right one here.
 *
 * Getting it wrong is worth understanding, because it does not fail politely.
 * A template merely lands off-centre, since the *tag* draws it and uses its own
 * stride. An uploaded image is a raw framebuffer whose stride this file
 * chooses, so a mismatch shifts every row against the one above it and the
 * picture arrives as diagonal hash. pushImage() refuses a wrongly-sized buffer,
 * which catches the common case, but a wrong choice here is still the one
 * mistake that produces garbage rather than a message. */
export const PANELS = {
  high: { key: 'high', w: 122, h: 250, label: '122 × 250 — HINK-E0213A53' },
  low:  { key: 'low',  w: 104, h: 212, label: '104 × 212 — HINK-E0213A41' },
};

for (const p of Object.values(PANELS)) {
  p.wbytes = (p.w + 7) >> 3;
  p.bytes  = p.wbytes * p.h;
}

let activeGeom = PANELS.high;

/** The panel geometry new Panels default to. */
export function activePanel() { return activeGeom; }

/** Choose the panel this session is talking to. Returns the new geometry.
 *  Panels already constructed keep the geometry they were built with. */
export function setActivePanel(key) {
  const p = PANELS[key];
  if (!p) throw new Error(`Unknown panel '${key}'.`);
  activeGeom = p;
  return p;
}

/* The three font tables, generated into font_data.js by tools/genfont.py
 * alongside the firmware's own copy. Both come from one run of the generator,
 * because a preview that disagrees with the panel about what a character looks
 * like is worse than no preview - see the header of that tool. */

/* Font ids, generated alongside the tables. Re-exported because callers and
 * tests already import everything else from this module. */
export const FONT_5X7 = EPD_FONT_5X7;
export const FONT_16X24 = EPD_FONT_16X24;
export const FONT_CJK16 = EPD_FONT_CJK16;

/* Glyph record for a codepoint, or null. The index is sorted, so bisect. */
function findGlyph(font, cp) {
  const idx = font.index;
  let lo = 0, hi = idx.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (idx[mid][0] < cp) lo = mid + 1;
    else if (idx[mid][0] > cp) hi = mid;
    else return idx[mid];
  }
  return null;
}

/* Cell width of one codepoint, excluding the 1px gap that follows it. Per
 * glyph rather than per font: the 16x16 face stores ASCII at 8px, so '2026年'
 * advances 8 four times and then 16. Ports glyph_w() in epd_gfx.c. */
function glyphW(font, cp) {
  const g = findGlyph(font, cp);
  return g ? g[2] : font.index[0][2];
}

/* Text for {W}, {M} and {P} per LOCALE(). Mirrors WDAY_NAME / MONTH_NAME /
 * AMPM_NAME in epd_cmdparser.c, indexed the same way - en, zh, ja.
 *
 * Chinese and Japanese take the numeric month form, 7月 rather than 七月,
 * because that is what a printed calendar shows in either language. Every
 * character here is in tools/glyphs.txt; the missing-glyph warning below is
 * what catches it if that stops being true. */
export const LOCALES = ['en', 'zh', 'ja'];

const WDAY_NAME = [
  ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'],
  ['星期日', '星期一', '星期二', '星期三', '星期四', '星期五', '星期六'],
  ['日曜日', '月曜日', '火曜日', '水曜日', '木曜日', '金曜日', '土曜日'],
];
const MONTH_NAME = [
  ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN',
   'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'],
  ['1月', '2月', '3月', '4月', '5月', '6月',
   '7月', '8月', '9月', '10月', '11月', '12月'],
  ['1月', '2月', '3月', '4月', '5月', '6月',
   '7月', '8月', '9月', '10月', '11月', '12月'],
];
const AMPM_NAME = [['AM', 'PM'], ['上午', '下午'], ['午前', '午後']];

/* The firmware's clock counts seconds from 2000-01-01; Date works in Unix
 * seconds. The tag has no notion of a timezone, so local wall-clock time is
 * sent as if it were UTC and read back the same way - see tagSecondsNow(). */
export const EPOCH_2000 = 946684800;

/** Seconds since 2000-01-01 for the browser's *local* wall clock. */
export function tagSecondsNow(now = new Date()) {
  return Math.floor(now.getTime() / 1000) - now.getTimezoneOffset() * 60
         - EPOCH_2000;
}

const isLeap = (y) => (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;
const MDAYS_BEFORE = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
const MDAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

/* Ports weeks_in_year() from epd_time.c - 53 ISO weeks iff the year ends on a
 * Thursday, or the previous one did on a Wednesday. */
function weeksInYear(y) {
  const p = (y + ((y / 4) | 0) - ((y / 100) | 0) + ((y / 400) | 0)) % 7;
  const q = y - 1;
  const r = (q + ((q / 4) | 0) - ((q / 100) | 0) + ((q / 400) | 0)) % 7;
  return (p === 4 || r === 3) ? 53 : 52;
}

/** Break tag-seconds into the fields the {} variables expose. */
export function tagTime(secs) {
  const d = new Date((secs + EPOCH_2000) * 1000);
  const year = d.getUTCFullYear();
  const month = d.getUTCMonth() + 1;
  const day = d.getUTCDate();
  const wday = d.getUTCDay();
  const leap = isLeap(year);

  const yday = MDAYS_BEFORE[month - 1] + day + (leap && month > 2 ? 1 : 0);
  const ydays = leap ? 366 : 365;
  const mdays = MDAYS[month - 1] + (leap && month === 2 ? 1 : 0);

  /* ISO 8601: weeks run Monday-Sunday and belong to the year holding their
   * Thursday, so early January can land in the previous year's week 52/53 -
   * which is why wyear exists separately. Mirrors iso_week() in epd_time.c. */
  const isoWday = wday === 0 ? 7 : wday;
  let week = Math.floor((yday - isoWday + 10) / 7);
  let wyear = year;
  if (week < 1) {
    wyear = year - 1;
    week = weeksInYear(wyear);
  } else if (week > weeksInYear(year)) {
    wyear = year + 1;
    week = 1;
  }

  return {
    year, month, day, wday, yday, ydays, mdays, week, wyear,
    hour: d.getUTCHours(), min: d.getUTCMinutes(), sec: d.getUTCSeconds(),
    u: secs,
  };
}

/* ------------------------------------------------------------------ */
/* Framebuffer + primitives - the epd_gfx.c port.                      */
/* ------------------------------------------------------------------ */

export class Panel {
  /* Geometry is captured here rather than read from the module on every access,
   * so a Panel stays self-consistent even if the active panel is switched while
   * it is alive - and so the tests can build both sizes side by side. */
  constructor(geom = activeGeom) {
    this.geom = geom;
    this.fb = new Uint8Array(geom.bytes);
    this.rot = 0;
    this.clear(0);
  }

  get width()  { return (this.rot & 1) ? this.geom.h : this.geom.w; }
  get height() { return (this.rot & 1) ? this.geom.w : this.geom.h; }

  setRotation(r) { this.rot = r & 3; }

  /* Rotated (drawing) coords -> native panel coords. Mirrors fb_set(). */
  _map(x, y) {
    const { w, h } = this.geom;
    switch (this.rot) {
      case 1:  return [w - 1 - y, x];
      case 2:  return [w - 1 - x, h - 1 - y];
      case 3:  return [y, h - 1 - x];
      default: return [x, y];
    }
  }

  set(x, y, color) {
    /* Clip in the rotated frame, before the transform, exactly as the firmware
     * does - so a shape clips against what the author can actually see. */
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
    const [px, py] = this._map(x, y);
    const idx = py * this.geom.wbytes + (px >> 3);
    const mask = 0x80 >> (px & 7);
    if (color) this.fb[idx] |= mask;      /* 1 = white */
    else       this.fb[idx] &= ~mask;     /* 0 = black */
  }

  get(x, y) {
    const [px, py] = this._map(x, y);
    const idx = py * this.geom.wbytes + (px >> 3);
    return (this.fb[idx] & (0x80 >> (px & 7))) ? 1 : 0;
  }

  clear(color) { this.fb.fill(color ? 0xff : 0x00); }

  /* Mirrors epd_gfx_invert(): corners inclusive, either order, clipped in the
   * rotated frame per pixel. Written against the framebuffer directly rather
   * than as get()-then-set() because get() does not clip. */
  invert(x1, y1, x2, y2) {
    if (x1 > x2) { const t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { const t = y1; y1 = y2; y2 = t; }
    for (let y = y1; y <= y2; y++) {
      for (let x = x1; x <= x2; x++) {
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) continue;
        const [px, py] = this._map(x, y);
        this.fb[py * this.geom.wbytes + (px >> 3)] ^= 0x80 >> (px & 7);
      }
    }
  }

  blob(x, y, color, pix) {
    const half = Math.trunc(pix / 2);
    for (let dy = 0; dy < pix; dy++)
      for (let dx = 0; dx < pix; dx++)
        this.set(x - half + dx, y - half + dy, color);
  }

  line(x1, y1, x2, y2, color, pix) {
    const sx = x1 < x2 ? 1 : -1;
    const sy = y1 < y2 ? 1 : -1;
    const dx = Math.abs(x2 - x1);
    const dy = -Math.abs(y2 - y1);       /* negative magnitude, as in the C */
    let err = dx + dy;
    if (pix < 1) pix = 1;

    for (;;) {
      this.blob(x1, y1, color, pix);
      if (x1 === x2 && y1 === y2) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x1 += sx; }
      if (e2 <= dx) { err += dx; y1 += sy; }
    }
  }

  rect(x1, y1, x2, y2, color, pix, filled) {
    if (filled) {
      for (let y = y1; y <= y2; y++) this.line(x1, y, x2, y, color, 1);
    } else {
      this.line(x1, y1, x2, y1, color, pix);
      this.line(x1, y2, x2, y2, color, pix);
      this.line(x1, y1, x1, y2, color, pix);
      this.line(x2, y1, x2, y2, color, pix);
    }
  }

  circle(x0, y0, r, color, pix, filled) {
    let x = r, y = 0, err = 0;
    while (x >= y) {
      if (filled) {
        this.line(x0 - x, y0 + y, x0 + x, y0 + y, color, 1);
        this.line(x0 - x, y0 - y, x0 + x, y0 - y, color, 1);
        this.line(x0 - y, y0 + x, x0 + y, y0 + x, color, 1);
        this.line(x0 - y, y0 - x, x0 + y, y0 - x, color, 1);
      } else {
        for (const [bx, by] of [[x, y], [y, x], [-y, x], [-x, y],
                                [-x, -y], [-y, -x], [y, -x], [x, -y]])
          this.blob(x0 + bx, y0 + by, color, pix);
      }
      y++;
      if (err <= 0) err += 2 * y + 1;
      if (err > 0) { x--; err -= 2 * x + 1; }
    }
  }

  text(x, y, str, fore, back, scale, font = EPD_FONT_5X7) {
    if (scale < 1) scale = 1;
    if (font < 0 || font >= FONTS.length) font = EPD_FONT_5X7;

    const f = FONTS[font];
    const gh = f.h;
    let cursor = x;

    /* for..of iterates codepoints, which is what the firmware's utf8_next()
     * yields - not UTF-16 units, which would split anything outside the BMP
     * into two glyphs the tag would draw as one. */
    for (const ch of str) {
      const g = findGlyph(f, ch.codePointAt(0));
      const gw = g ? g[2] : f.index[0][2];

      for (let col = 0; col < gw; col++) {
        for (let row = 0; row < gh; row++) {
          /* Column-major, LSB = top, bpc bytes per column: byte (row / 8),
           * bit (row % 8). Mirrors the same expression in epd_gfx_text(). */
          const bits = g ? f.bits[g[1] + col * f.bpc + (row >> 3)] : 0x00;
          const color = ((bits >> (row & 7)) & 1) ? fore : back;
          for (let sx = 0; sx < scale; sx++)
            for (let sy = 0; sy < scale; sy++)
              this.set(cursor + col * scale + sx, y + row * scale + sy, color);
        }
      }
      cursor += (gw + 1) * scale;       /* glyph width + 1px gap */
    }
  }
}

/** Rendered width of `str` at `scale` - no gap after the last glyph.
 *  Handy for centring text, which is most of what face authoring is.
 *
 *  Ports epd_gfx_text_width(). Takes the string rather than a glyph count,
 *  which is what it took while every glyph in a font was the same width: the
 *  16x16 face mixes 8px ASCII with 16px CJK, so a count no longer determines
 *  a width. Empty text is 0 px - not -scale, which is what the old
 *  ((w+1)n - 1) gave and what a naive port would inherit. */
export function textWidth(str, scale, font = EPD_FONT_5X7) {
  if (scale < 1) scale = 1;
  if (font < 0 || font >= FONTS.length) font = EPD_FONT_5X7;

  const f = FONTS[font];
  let w = 0;
  for (const ch of str) w += glyphW(f, ch.codePointAt(0)) + 1;
  return w === 0 ? 0 : (w - 1) * scale;
}

export const TEXT_HEIGHT = (scale, font = EPD_FONT_5X7) =>
  (FONTS[font] ? FONTS[font].h : FONTS[EPD_FONT_5X7].h) * scale;

/* ------------------------------------------------------------------ */
/* {} variable expansion - the expand_vars() port.                     */
/* ------------------------------------------------------------------ */

/**
 * Numeric value of a {} variable, or undefined for the text-valued names
 * ({W}, {M}, {P}, {VER}) and anything unknown.
 *
 * Ports var_num() in epd_cmdparser.c, and is shared with the expression
 * evaluator for the same reason it is shared there: a name must not mean one
 * thing inside FONT text and another in a coordinate.
 */
export function varNum(name, tm) {
  switch (name) {
    case 'y': return tm.year;
    case 'm': return tm.month;
    case 'd': return tm.day;
    case 'H': return tm.hour;
    case 'N': return tm.min;
    case 'S': return tm.sec;
    case 'w': return tm.wday;
    /* Lower case is the position, upper case the length it runs against:
     * {d}/{D} within the month, {j}/{J} within the year. */
    case 'j': return tm.yday;
    case 'J': return tm.ydays;
    case 'D': return tm.mdays;
    /* {D} was called {L} before the pairing above existed - see the same case
     * in var_num() for why it is still accepted and still undocumented. */
    case 'L': return tm.mdays;
    case 'V': return tm.week;
    case 'G': return tm.wyear;
    case 'h': return (tm.hour % 12) || 12;   /* midnight and noon are 12 */
    case 'u': return tm.u;
    /* Panel temperature, whole degrees Celsius. undefined unless a caller
     * supplied one, so {T} renders literally in a preview that has no reading
     * to show - which is exactly what the firmware does when nothing has
     * called epd_cmd_set_temp(). The two have to agree or the byte-identity
     * test would be comparing a number against the literal text. */
    case 'T': return tm.temp;
    /* Battery: charge in percent and terminal voltage in millivolts, from one
     * reading. Same undefined-unless-supplied rule as {T} above, and for the
     * same parity reason - the firmware does not know either until something
     * has called epd_cmd_set_batt(). Multi-letter names work here for free
     * because this switches on the whole string; var_num() in the firmware
     * needs an explicit branch, and must not let {VER} reach it. */
    case 'BAT': return tm.battPct;
    case 'VCC': return tm.battMv;
    default:  return undefined;
  }
}

export function expandVars(input, secs, env = {}) {
  const tm = tagTime(secs);
  Object.assign(tm, env);
  let out = '';
  let i = 0;

  while (i < input.length) {
    if (input[i] !== '{') { out += input[i++]; continue; }

    /* {name} or {name:0Wd} */
    const m = /^\{([A-Za-z]*)(?::(0?)(\d*)d?)?\}/.exec(input.slice(i));
    if (!m) { out += input[i++]; continue; }   /* malformed - emit literally */

    const [tok, name, zero, widthStr] = m;
    const width = widthStr ? parseInt(widthStr, 10) : 0;
    const n = varNum(name, tm);

    if (n !== undefined) {
      /* Zero padding puts the sign first, as printf("%03d", -5) gives "-05"
       * and not "0-5" - String(-5).padStart(3,'0') would give the latter.
       * Matches append_int() in epd_cmdparser.c; the byte-identity test covers
       * a negative, which only became reachable when {T} arrived. */
      if (zero && n < 0) {
        out += '-' + String(-n).padStart(width > 0 ? width - 1 : 0, '0');
      } else {
        out += String(n).padStart(width, zero ? '0' : ' ');
      }
    } else if (name === 'W') {
      out += WDAY_NAME[tm.locale | 0][tm.wday % 7];
    } else if (name === 'M') {
      out += MONTH_NAME[tm.locale | 0][(tm.month - 1) % 12];
    } else if (name === 'P') {
      out += AMPM_NAME[tm.locale | 0][tm.hour < 12 ? 0 : 1];
    } else if (name === 'VER') {
      out += 'HEMA1';
    } else {
      out += tok;                              /* unknown - copy through */
    }
    i += tok.length;
  }
  return out;
}

/* ------------------------------------------------------------------ */
/* Script execution - the dispatch_line() port.                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Numeric arguments - the parse_int()/parse_expr() port.              */
/*                                                                     */
/* Arguments are expressions: integers, {} variables, + - * / %,        */
/* parentheses and unary minus. Everything is int32 and truncating, and  */
/* malformed input yields 0 rather than throwing, exactly as the        */
/* firmware does - see the header comment above parse_int() in          */
/* epd_cmdparser.c.                                                     */
/* ------------------------------------------------------------------ */

const EXPR_MAX_DEPTH = 8;

/* Force C's int32 wrap-around; JS numbers are doubles and would not. */
const i32 = (v) => v | 0;

const skipSpace = (s, i) => {
  while (s[i] === ' ') i++;
  return i;
};

function parseFactor(s, i, depth, tm) {
  i = skipSpace(s, i);

  if (s[i] === '-') {
    const r = parseFactor(s, i + 1, depth, tm);
    return { v: i32(-r.v), i: r.i };
  }
  if (s[i] === '+') return parseFactor(s, i + 1, depth, tm);

  if (s[i] === '(') {
    let v = 0, j = i + 1;
    /* Past the cap the sub-expression is skipped rather than recursed, which
     * is what the firmware does to keep off its 1.7 KB stack. */
    if (depth < EXPR_MAX_DEPTH) {
      const r = parseExpr(s, j, depth + 1, tm);
      v = r.v; j = r.i;
    }
    j = skipSpace(s, j);
    if (s[j] === ')') j++;
    return { v, i: j };
  }

  if (s[i] === '{') {
    let j = i + 1, name = '';
    while (j < s.length && s[j] !== '}' && name.length < 11) name += s[j++];
    if (s[j] === '}') {
      const v = varNum(name, tm);
      return { v: v === undefined ? 0 : i32(v), i: j + 1 };
    }
    return { v: 0, i: i + 1 };     /* unterminated - step over the brace */
  }

  let v = 0, j = i;
  while (s[j] >= '0' && s[j] <= '9') {
    v = i32(v * 10 + (s.charCodeAt(j) - 48));
    j++;
  }
  return { v, i: j };
}

function parseTerm(s, i, depth, tm) {
  let { v, i: j } = parseFactor(s, i, depth, tm);

  for (;;) {
    const k = skipSpace(s, j);
    const op = s[k];
    if (op !== '*' && op !== '/' && op !== '%') break;

    const r = parseFactor(s, k + 1, depth, tm);
    j = r.i;
    if (op === '*') v = Math.imul(v, r.v);     /* exact int32, unlike v*r.v */
    else if (r.v === 0) v = 0;                 /* divide by zero -> 0 */
    else if (op === '/') v = i32(v / r.v);     /* truncates toward zero */
    else v = i32(v % r.v);
  }
  return { v, i: j };
}

function parseExpr(s, i, depth, tm) {
  let { v, i: j } = parseTerm(s, i, depth, tm);

  for (;;) {
    const k = skipSpace(s, j);
    const op = s[k];
    if (op !== '+' && op !== '-') break;

    const r = parseTerm(s, k + 1, depth, tm);
    j = r.i;
    v = i32(op === '+' ? v + r.v : v - r.v);
  }
  return { v, i: j };
}

/** Evaluate one standalone argument. Missing arguments are 0, as in the
 *  firmware. Exposed for tests; runScript() uses Args below. */
export function evalArg(text, tm) {
  if (text === undefined) return 0;
  return parseExpr(text, 0, 0, tm).v;
}

/**
 * Reads a command's arguments the way the firmware does: one cursor walking
 * the text, each int()/str() consuming what it needs plus a trailing ',' or
 * ')'.
 *
 * This is deliberately not "split on commas, then evaluate each piece". The
 * two agree on well-formed input and diverge the moment an argument does not
 * consume cleanly: given POINT(1 2,3,0,1) the firmware reads 1, stops at the
 * space, and its *next* read starts at the 2 - so it sees 1,2,3 where a
 * comma-split sees 1,3,0. Same class of drift with a ')' inside an expression.
 * Since the whole point of this file is to be a port, it parses like the port.
 */
const NAME_RE = /^[A-Za-z_][A-Za-z_0-9]*/;

/** Ports at_named(): is the cursor looking at `name=` rather than a value?
 *  No expression can begin with a letter, so this is never ambiguous. */
function atNamed(s, i) {
  const rest = s.slice(skipSpace(s, i));
  const m = NAME_RE.exec(rest);
  return m ? /^\s*=/.test(rest.slice(m[0].length)) : false;
}

/**
 * Ports find_named(): the index just past `name=`, or -1.
 *
 * Only matches at the start of a top-level argument, so a quoted string
 * ("FONT(...,'scale=3')") and anything inside parentheses are both immune. The
 * '=' check is what keeps "color" from matching "colors=1".
 */
function findNamed(args, name) {
  let argStart = true, inStr = false, depth = 0, i = 0;

  while (i < args.length) {
    const c = args[i];

    if (inStr) {
      if (c === "'") inStr = false;
      i++;
      continue;
    }

    if (argStart) {
      let j = skipSpace(args, i);
      if (args.startsWith(name, j)) {
        j = skipSpace(args, j + name.length);
        if (args[j] === '=') return j + 1;
      }
      argStart = false;
    }

    if (c === "'") { inStr = true; i++; continue; }
    if (c === '(') { depth++; i++; continue; }
    if (c === ')') {
      if (depth === 0) return -1;          /* end of the argument list */
      depth--; i++; continue;
    }
    if (c === ',' && depth === 0) { argStart = true; i++; continue; }
    i++;
  }
  return -1;
}

/**
 * Reads a command's arguments the way the firmware does: one cursor walking the
 * positional list, plus a re-scan per named option.
 *
 * The cursor walk is deliberately not "split on commas, then evaluate each
 * piece". The two agree on well-formed input and diverge the moment an argument
 * does not consume cleanly: given POINT(1 2,3) the firmware reads 1, stops at
 * the space, and its *next* read starts at the 2 - so it sees 1,2 where a
 * comma-split sees 1,3. Same class of drift with a ')' inside an expression.
 * Since the whole point of this file is to be a port, it parses like the port.
 */
class Args {
  constructor(text, tm) {
    this.s = text;
    this.i = 0;
    this.tm = tm;
  }

  /** Ports parse_int(). Stops at an option rather than consuming it. */
  int() {
    if (atNamed(this.s, this.i)) return 0;
    const r = parseExpr(this.s, this.i, 0, this.tm);
    let j = skipSpace(this.s, r.i);
    if (this.s[j] === ',' || this.s[j] === ')') j++;
    this.i = j;
    return r.v;
  }

  ints(n) {
    const out = [];
    for (let k = 0; k < n; k++) out.push(this.int());
    return out;
  }

  /** Ports parse_string(). CMD_TEXT_MAX is 64, so 63 chars plus a NUL. */
  str() {
    if (atNamed(this.s, this.i)) return '';
    let j = skipSpace(this.s, this.i);
    let out = '';

    if (this.s[j] === "'") {
      j++;
      while (j < this.s.length && this.s[j] !== "'" && out.length < 63) {
        out += this.s[j++];
      }
      if (this.s[j] === "'") j++;
    }

    j = skipSpace(this.s, j);
    if (this.s[j] === ',' || this.s[j] === ')') j++;
    this.i = j;
    return out;
  }

  /** A bare unquoted token, for LOCALE()'s language code.
   *
   * The firmware reads two characters and insists the next one is ')', which
   * is not quite this - but the two agree on every input that matters: every
   * code it accepts is two letters, and anything else is rejected by both,
   * one as a note_err() and the other as a warning here. */
  token() {
    let j = skipSpace(this.s, this.i);
    let out = '';
    while (j < this.s.length && this.s[j] !== ')' && this.s[j] !== ','
           && out.length < 16) {
      out += this.s[j++];
    }
    if (this.s[j] === ',' || this.s[j] === ')') j++;
    this.i = j;
    return out.trim();
  }

  /** Ports named_int(): an option's value, or `dflt` if it was not given. */
  named(name, dflt) {
    const at = findNamed(this.s, name);
    return at < 0 ? dflt : parseExpr(this.s, at, 0, this.tm).v;
  }

  /**
   * Every option name actually present, for the preview's benefit only - the
   * firmware never enumerates them, it only ever looks up the ones it wants.
   * Used to tell an author that `colour=` will be ignored, which is otherwise
   * indistinguishable from it having had no effect.
   */
  names() {
    const out = [];
    let argStart = true, inStr = false, depth = 0, i = 0;

    while (i < this.s.length) {
      const c = this.s[i];
      if (inStr) { if (c === "'") inStr = false; i++; continue; }

      if (argStart) {
        const j = skipSpace(this.s, i);
        const m = NAME_RE.exec(this.s.slice(j));
        if (m && /^\s*=/.test(this.s.slice(j + m[0].length))) out.push(m[0]);
        argStart = false;
      }

      if (c === "'") { inStr = true; i++; continue; }
      if (c === '(') { depth++; i++; continue; }
      if (c === ')') { if (depth === 0) break; depth--; i++; continue; }
      if (c === ',' && depth === 0) { argStart = true; i++; continue; }
      i++;
    }
    return out;
  }
}

/* Every command dispatch_line() and handle_line() recognise. Used only to tell
 * a case mistake ("rect(") apart from a genuinely unknown command, which are
 * the same silent no-op on the tag but very different mistakes to make. */
/* The options each command understands. The firmware never enumerates these -
 * it looks up the ones it wants and ignores the rest, because it has nowhere
 * to report a typo to. Here they exist so the preview can say that `colour=`
 * will do nothing, which on the panel is indistinguishable from having done
 * nothing. Keep in step with named_int() in dispatch_line(). */
export const OPTIONS = {
  CLEAR:  [],
  POINT:  ['color'],
  LINE:   ['color', 'width'],
  RECT:   ['color', 'width', 'fill'],
  CIRCLE: ['color', 'width', 'fill'],
  TEXT:   ['color', 'bg', 'scale', 'align', 'font'],
  ROTATE: [],
  INVERT: [],
  EVERY:  [],
  LOCALE: [],
  TIME:   [],
  RESET:  [],
};
const COMMANDS = new Set(Object.keys(OPTIONS));

/* Commands that existed under another name. Worth naming specifically: "FONT()
 * is not implemented" sends an author looking for a missing feature, when what
 * they need is one word changed. */
const RENAMED = { FONT: 'TEXT' };

/* Upper bound on EVERY(), matching CMD_EVERY_MAX in epd_cmdparser.c. A day;
 * beyond that the interval stops meaning anything a shelf label cares about. */
export const EVERY_MAX = 1440;

/**
 * Run a script against a panel.
 * Returns { warnings: [{line, text, msg}] } - authoring aid only; the firmware
 * itself ignores everything it doesn't recognise.
 */
export function runScript(panel, script, secs, env = {}) {
  const warnings = [];
  /* Default to a repaint a minute, and reset per run, so the interval is a
   * property of this script and nothing carried over - matching epd_cmd_run(),
   * which resets s_every_min for the same reason. */
  let every = 1;
  /* epd_cmd_run() ends a line on '\n' *or* '\r', so a lone CR is a separator
   * and not part of the command. Splitting on '\n' alone made the preview
   * treat "CLEAR(1)\rRECT(...)" as one unparseable line while the tag ran
   * both. */
  const lines = script.split(/\r\n|[\n\r]/);

  /* One clock reading for the whole script, as epd_cmd_run() takes. Reading it
   * per reference would let a script that straddles a second boundary render
   * {S} inconsistently between its own lines. */
  const tm = tagTime(secs);
  /* Carried on tm rather than passed alongside it, so every consumer that
   * already takes tm - varNum(), the expression evaluator, expandVars() -
   * sees it without a second parameter threaded through each one.
   *
   * One object rather than a parameter each: {T} was the first, the battery
   * added two more, and LOCALE() adds a fourth that - unlike the others - the
   * script itself can change as it runs. A positional list was going to keep
   * growing and every caller would have to count undefineds to reach the one
   * it cared about. */
  Object.assign(tm, env);
  /* LOCALE() defaults to English and resets per run, matching epd_cmd_run():
   * dropping it from a face must not leave the previous face's language
   * standing. Carried on tm so expandVars() below sees it. */
  tm.locale = 0;

  lines.forEach((raw, n) => {
    /* Leading whitespace is skipped by the firmware too (skip_ws), so an
     * indented line runs on the tag exactly as it previews here. */
    const line = raw.trim();
    /* The firmware has no comment syntax - a '#' line simply matches no
     * command and is ignored. Same result, but note it still occupies script
     * buffer on the tag. */
    if (!line || line.startsWith('#')) return;

    /* The name must be followed *immediately* by '(': the firmware matches a
     * literal "RECT(" prefix, so "RECT (" and "rect(" are not commands to it.
     * Matching loosely here made the preview draw shapes the tag ignored. */
    const m = /^([A-Za-z_]+)\(/.exec(line);
    if (!m) {
      warnings.push({ line: n + 1, text: line, msg: 'not a command' });
      return;
    }
    const cmd = m[1];                       /* case preserved, as in the C */
    const a = new Args(line.slice(m[0].length), tm);

    switch (cmd) {
      case 'CLEAR':
        panel.clear(a.int());
        break;

      case 'POINT': {
        const [x, y] = a.ints(2);
        panel.set(x, y, a.named('color', 0));
        break;
      }

      case 'LINE': {
        const [x1, y1, x2, y2] = a.ints(4);
        panel.line(x1, y1, x2, y2, a.named('color', 0), a.named('width', 1));
        break;
      }

      case 'RECT': {
        const [x1, y1, x2, y2] = a.ints(4);
        panel.rect(x1, y1, x2, y2, a.named('color', 0),
                   a.named('width', 1), a.named('fill', 0));
        break;
      }

      case 'CIRCLE': {
        const [x, y, r] = a.ints(3);
        panel.circle(x, y, r, a.named('color', 0),
                     a.named('width', 1), a.named('fill', 0));
        break;
      }

      case 'TEXT': {
        /* TEXT(x, y, 'text', ...). The text is positional because the
         * command is meaningless without it. */
        const [x, y] = a.ints(2);
        const str = a.str();
        const scale = a.named('scale', 1);
        const shown = expandVars(str, secs, tm);

        /* align= moves the anchor: x is the left edge at 0, the centre at 1,
         * the right edge at 2. Measured after expansion, because the width of
         * "{H:02d}:{N:02d}" is not the width of "09:41". Math.trunc to match
         * the firmware's integer division - both floor for the positive
         * widths this can produce, but the intent should not rest on that. */
        const align = a.named('align', 0);
        const font = a.named('font', EPD_FONT_5X7);
        let tx = x;
        if (align !== 0) {
          const w = textWidth(shown, scale, font);
          tx -= align === 1 ? Math.trunc(w / 2) : w;
        }

        /* No font carries every character: the large one is digits and ':',
         * and the 16x16 one holds the characters tools/glyphs.txt lists and
         * no more. The tag draws anything else blank without complaint - it
         * has nowhere to complain to - so this is the only place an author
         * finds out before looking at the panel. */
        const table = FONTS[font];
        if (table) {
          const missing = [...new Set(shown)]
            .filter((c) => !findGlyph(table, c.codePointAt(0)));
          if (missing.length) {
            warnings.push({
              line: n + 1, text: line,
              msg: `font=${font} has no glyph for `
                 + `${missing.map((c) => `'${c}'`).join(', ')}`
                 + ' - the tag will leave a gap there',
            });
          }
        }

        panel.text(tx, y, shown, a.named('color', 0), a.named('bg', 1), scale,
                   font);
        break;
      }

      case 'ROTATE': {
        /* Degrees only. The index form the vendor also accepted overlapped at
         * exactly the confusing values - ROTATE(3) meant 270 - so it is
         * refused and reported rather than taken as 3 degrees. */
        const deg = a.int();
        const quarter = { 0: 0, 90: 1, 180: 2, 270: 3 }[deg];
        if (quarter === undefined) {
          warnings.push({
            line: n + 1, text: line,
            msg: `ROTATE(${deg}) is not a quarter turn - use 0, 90, 180 or 270`
               + (deg >= 1 && deg <= 3
                  ? `. ROTATE(${deg}) used to mean ${deg * 90} degrees; it does not now`
                  : '') + ', and the tag will leave the rotation unchanged',
          });
          break;
        }
        panel.setRotation(quarter);
        break;
      }

      case 'INVERT': {
        /* INVERT(x, y, w, h) - width and height, not a second corner. */
        const [x, y, w, h] = a.ints(4);
        if (w > 0 && h > 0) panel.invert(x, y, x + w - 1, y + h - 1);
        break;
      }

      case 'EVERY': {
        /* Draws nothing - it sets how often the tag repaints. Reported back
         * to the caller so the editor can say so, since it is otherwise
         * invisible in a preview that renders one instant. Clamped exactly as
         * the firmware clamps it, or the editor would promise an interval the
         * tag will not honour. */
        let n = a.int();
        if (n < 1) n = 1;
        if (n > EVERY_MAX) n = EVERY_MAX;
        every = n;
        break;
      }

      case 'LOCALE': {
        /* Draws nothing; it selects the language {W}, {M} and {P} render in.
         * An unknown code is reported and leaves the locale alone, matching
         * the firmware - and matching ROTATE, where guessing would put a whole
         * face in the wrong script with nothing to go on. */
        const code = a.token().toLowerCase();
        const i = LOCALES.indexOf(code);
        if (i < 0) {
          warnings.push({
            line: n + 1, text: line,
            msg: `LOCALE(${code || ''}) is not a language this understands`
               + ` - use ${LOCALES.join(', ')}`,
          });
        } else {
          tm.locale = i;
        }
        break;
      }

      case 'TIME':
      case 'RESET':
        /* Control commands: applied on arrival by the firmware and never
         * stored, so they draw nothing and the preview ignores them. */
        break;

      default:
        warnings.push({
          line: n + 1, text: line,
          msg: RENAMED[cmd.toUpperCase()]
            ? `${cmd}() is now ${RENAMED[cmd.toUpperCase()]}() - the tag will `
              + 'ignore this line'
            : COMMANDS.has(cmd.toUpperCase())
            ? `${cmd}() must be written ${cmd.toUpperCase()}() - commands are `
              + 'case-sensitive, and the tag will ignore this line'
            : `${cmd}() is not implemented - the tag will ignore it`,
        });
        return;
    }

    /* An option the command does not read is silently dropped by the tag, so
     * it is worth more than a shrug here: a misspelt or misplaced option looks
     * exactly like one that had no visible effect. */
    const known = OPTIONS[cmd];
    for (const opt of a.names()) {
      if (!known.includes(opt)) {
        warnings.push({
          line: n + 1, text: line,
          msg: known.length
            ? `${cmd}() has no ${opt}= option - it accepts `
              + `${known.map((k) => `${k}=`).join(', ')}`
            : `${cmd}() takes no options, so ${opt}= is ignored`,
        });
      }
    }
  });

  return { warnings, every };
}

/** Draw a panel onto a canvas at `zoom`, in the panel's current orientation. */
export function paint(panel, canvas, zoom) {
  const w = panel.width, h = panel.height;
  canvas.width = w * zoom;
  canvas.height = h * zoom;

  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(w, h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      /* E-paper white is a warm off-white, black a soft charcoal - a pure
       * #fff/#000 preview flatters the panel more than it deserves. */
      const on = panel.get(x, y);
      const [r, g, b] = on ? [0xf4, 0xf2, 0xea] : [0x22, 0x22, 0x24];
      const i = (y * w + x) * 4;
      img.data[i] = r; img.data[i + 1] = g; img.data[i + 2] = b;
      img.data[i + 3] = 255;
    }
  }

  /* Blit at 1:1 into a scratch canvas, then scale up with smoothing off, so
   * one panel pixel stays one crisp square. */
  const tmp = document.createElement('canvas');
  tmp.width = w; tmp.height = h;
  tmp.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, 0, 0, canvas.width, canvas.height);
}
