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

/* 5x7 fallback font, transcribed from FONT_5X7 in epd_gfx.c.
 * One byte per column, LSB = top row. Uppercase only; lowercase folds up. */
const FONT = {
  ' ': [0x00, 0x00, 0x00, 0x00, 0x00],
  '-': [0x08, 0x08, 0x08, 0x08, 0x08],
  '/': [0x60, 0x10, 0x08, 0x04, 0x03],
  ':': [0x00, 0x36, 0x36, 0x00, 0x00],
  '.': [0x00, 0x60, 0x60, 0x00, 0x00],
  ',': [0x00, 0x50, 0x30, 0x00, 0x00],
  '+': [0x08, 0x08, 0x3e, 0x08, 0x08],
  '%': [0x24, 0x64, 0x08, 0x13, 0x23],
  '*': [0x14, 0x08, 0x3e, 0x08, 0x14],
  '(': [0x00, 0x1c, 0x22, 0x41, 0x00],
  ')': [0x00, 0x41, 0x22, 0x1c, 0x00],
  "'": [0x00, 0x05, 0x03, 0x00, 0x00],
  '?': [0x02, 0x01, 0x51, 0x09, 0x06],
  '!': [0x00, 0x00, 0x5f, 0x00, 0x00],
  '=': [0x14, 0x14, 0x14, 0x14, 0x14],
  '~': [0x00, 0x07, 0x05, 0x07, 0x00],   /* stands in for the degree sign */
  '0': [0x3e, 0x51, 0x49, 0x45, 0x3e],
  '1': [0x00, 0x42, 0x7f, 0x40, 0x00],
  '2': [0x62, 0x51, 0x49, 0x49, 0x46],
  '3': [0x22, 0x41, 0x49, 0x49, 0x36],
  '4': [0x18, 0x14, 0x12, 0x7f, 0x10],
  '5': [0x2f, 0x49, 0x49, 0x49, 0x31],
  '6': [0x3c, 0x4a, 0x49, 0x49, 0x30],
  '7': [0x01, 0x71, 0x09, 0x05, 0x03],
  '8': [0x36, 0x49, 0x49, 0x49, 0x36],
  '9': [0x06, 0x49, 0x49, 0x29, 0x1e],
  'A': [0x7e, 0x11, 0x11, 0x11, 0x7e],
  'B': [0x7f, 0x49, 0x49, 0x49, 0x36],
  'C': [0x3e, 0x41, 0x41, 0x41, 0x22],
  'D': [0x7f, 0x41, 0x41, 0x22, 0x1c],
  'E': [0x7f, 0x49, 0x49, 0x49, 0x41],
  'F': [0x7f, 0x09, 0x09, 0x09, 0x01],
  'G': [0x3e, 0x41, 0x49, 0x49, 0x7a],
  'H': [0x7f, 0x08, 0x08, 0x08, 0x7f],
  'I': [0x00, 0x41, 0x7f, 0x41, 0x00],
  'J': [0x20, 0x40, 0x41, 0x3f, 0x01],
  'K': [0x7f, 0x08, 0x14, 0x22, 0x41],
  'L': [0x7f, 0x40, 0x40, 0x40, 0x40],
  'M': [0x7f, 0x02, 0x0c, 0x02, 0x7f],
  'N': [0x7f, 0x04, 0x08, 0x10, 0x7f],
  'O': [0x3e, 0x41, 0x41, 0x41, 0x3e],
  'P': [0x7f, 0x09, 0x09, 0x09, 0x06],
  'Q': [0x3e, 0x41, 0x51, 0x21, 0x5e],
  'R': [0x7f, 0x09, 0x19, 0x29, 0x46],
  'S': [0x46, 0x49, 0x49, 0x49, 0x31],
  'T': [0x01, 0x01, 0x7f, 0x01, 0x01],
  'U': [0x3f, 0x40, 0x40, 0x40, 0x3f],
  'V': [0x1f, 0x20, 0x40, 0x20, 0x1f],
  'W': [0x3f, 0x40, 0x38, 0x40, 0x3f],
  'X': [0x63, 0x14, 0x08, 0x14, 0x63],
  'Y': [0x07, 0x08, 0x70, 0x08, 0x07],
  'Z': [0x61, 0x51, 0x49, 0x45, 0x43],
};

/* 16x24 digits, transcribed from FONT_16X24 in epd_gfx.c. Both tables are
 * generated from the same ASCII art by tools/font16.py - edit there and
 * re-emit, do not hand-patch either copy.
 *
 * Column-major like the 5x7 table, but three bytes per column for 24 rows:
 * byte (row / 8), bit (row % 8), LSB at the top. Digits and ':' only; a
 * character that is missing draws blank, exactly as the firmware does. */
const FONT16 = {
  '0': [0xc0, 0xff, 0x1f, 0xe0, 0xff, 0x3f, 0xf0, 0xff, 0x7f, 0x70, 0x00, 0x70, 0x38, 0x00, 0xe0, 0x18, 0x00, 0xc0, 0x18, 0x00, 0xc0, 0x18, 0x00, 0xc0, 0x18, 0x00, 0xc0, 0x18, 0x00, 0xc0, 0x18, 0x00, 0xc0, 0x38, 0x00, 0xe0, 0x70, 0x00, 0x70, 0xf0, 0xff, 0x7f, 0xe0, 0xff, 0x3f, 0xc0, 0xff, 0x1f],
  '1': [0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x03, 0xc0, 0x80, 0x03, 0xc0, 0xc0, 0x01, 0xc0, 0xe0, 0x00, 0xc0, 0x70, 0x00, 0xc0, 0x70, 0x00, 0xc0, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0x00, 0x00, 0xc0, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
  '2': [0xc0, 0x00, 0xc0, 0xe0, 0x00, 0xc0, 0x60, 0x00, 0xe0, 0x30, 0x00, 0xf0, 0x30, 0x00, 0xf8, 0x30, 0x00, 0xdc, 0x30, 0x00, 0xce, 0x30, 0x00, 0xc7, 0x30, 0x80, 0xc3, 0x30, 0xc0, 0xc1, 0x30, 0xe0, 0xc0, 0x30, 0x70, 0xc0, 0x30, 0x38, 0xc0, 0x60, 0x1c, 0xc0, 0xe0, 0x0f, 0xc0, 0xc0, 0x07, 0xc0],
  '3': [0x80, 0x00, 0x10, 0xc0, 0x00, 0x70, 0xe0, 0x00, 0xf0, 0x60, 0x30, 0xe0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x30, 0xc0, 0x60, 0x78, 0xe0, 0xe0, 0xcf, 0xff, 0xc0, 0xcf, 0x7f, 0x80, 0x87, 0x1f],
  '4': [0x00, 0xc0, 0x01, 0x00, 0xe0, 0x01, 0x00, 0xb0, 0x01, 0x00, 0x98, 0x01, 0x00, 0x8c, 0x01, 0x00, 0x86, 0x01, 0x00, 0x83, 0x01, 0x80, 0x81, 0x01, 0xc0, 0x80, 0x01, 0x60, 0x80, 0x01, 0x30, 0x80, 0x01, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xf0, 0xff, 0xff, 0x00, 0x80, 0x01, 0x00, 0x80, 0x01],
  '5': [0xe0, 0x3f, 0x10, 0xe0, 0x3f, 0x70, 0xe0, 0x3f, 0xf0, 0x60, 0x38, 0xe0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x18, 0xc0, 0x60, 0x38, 0xc0, 0x60, 0x38, 0xe0, 0x60, 0xf0, 0xff, 0x60, 0xf0, 0x7f, 0x60, 0xe0, 0x1f],
  '6': [0x80, 0xff, 0x1f, 0xc0, 0xff, 0x3f, 0xe0, 0xff, 0x7f, 0xf0, 0x38, 0x70, 0x70, 0x18, 0xe0, 0x38, 0x18, 0xc0, 0x18, 0x18, 0xc0, 0x18, 0x18, 0xc0, 0x18, 0x18, 0xc0, 0x18, 0x18, 0xc0, 0x18, 0x18, 0xc0, 0x18, 0x38, 0xe0, 0x18, 0x38, 0x70, 0x18, 0xf0, 0x7f, 0x00, 0xf0, 0x3f, 0x00, 0xe0, 0x1f],
  '7': [0x30, 0x00, 0x80, 0x30, 0x00, 0xe0, 0x30, 0x00, 0xf8, 0x30, 0x00, 0x7e, 0x30, 0x80, 0x1f, 0x30, 0xc0, 0x07, 0x30, 0xe0, 0x01, 0x30, 0x70, 0x00, 0x30, 0x38, 0x00, 0x30, 0x1c, 0x00, 0x30, 0x0e, 0x00, 0x30, 0x07, 0x00, 0xb0, 0x03, 0x00, 0xf0, 0x01, 0x00, 0xf0, 0x00, 0x00, 0x70, 0x00, 0x00],
  '8': [0x00, 0x0f, 0x3f, 0x80, 0x9f, 0x7f, 0x80, 0xff, 0x7f, 0xc0, 0xf0, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0x60, 0xc0, 0xc0, 0xf0, 0xc0, 0x80, 0xff, 0x7f, 0x80, 0x9f, 0x7f, 0x00, 0x0f, 0x3f],
  '9': [0x80, 0x1f, 0x00, 0xc0, 0x3f, 0x00, 0xe0, 0x3f, 0x00, 0x60, 0x70, 0x00, 0x70, 0x60, 0x00, 0x30, 0x60, 0x00, 0x30, 0x60, 0x00, 0x30, 0x60, 0x00, 0x30, 0x60, 0x00, 0x30, 0x60, 0x00, 0x30, 0x60, 0x00, 0x70, 0x60, 0x00, 0x60, 0x70, 0x00, 0xe0, 0xff, 0xff, 0xc0, 0xff, 0xff, 0x80, 0xff, 0xff],
  ':': [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0f, 0x00, 0x0f, 0x0f, 0x00, 0x0f, 0x0f, 0x00, 0x0f, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
};

/* Font ids, matching EPD_FONT_* in epd_gfx.h. */
export const FONT_5X7 = 0;
export const FONT_16X24 = 1;

/* Cell width per font, excluding the 1px gap that follows each glyph. */
const fontW = (font) => (font === FONT_16X24 ? 16 : 5);

const WDAY_NAME = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];
const MONTH_NAME = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN',
                    'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];

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

  text(x, y, str, fore, back, scale, font = FONT_5X7) {
    if (scale < 1) scale = 1;
    const big = font === FONT_16X24;
    const gw = fontW(font);
    const gh = big ? 24 : 7;
    let cursor = x;

    for (const ch of str) {
      /* Case folding is the 5x7 table's affordance; the large table is digits
       * and ':' only, and folding would not find them anything. */
      const glyph = big ? (FONT16[ch] || null) : (FONT[ch.toUpperCase()] || null);

      for (let col = 0; col < gw; col++) {
        for (let row = 0; row < gh; row++) {
          /* Three bytes per column in the large font, one in the small - for
           * which row < 8 always, so this reduces to the old single-byte
           * form. Mirrors the same expression in epd_gfx_text(). */
          const bits = glyph
            ? (big ? glyph[col * 3 + (row >> 3)] : glyph[col])
            : 0x00;
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

/** Rendered width of `n` glyphs at `scale` - no gap after the last one.
 *  Handy for centring text, which is most of what face authoring is. */
/* Ports epd_gfx_text_width(). Takes a glyph count rather than the string,
 * since every caller here already has one. 0 glyphs is 0 px - not -scale,
 * which is what (6n-1) alone would give and what a naive port would inherit. */
export function textWidth(n, scale, font = FONT_5X7) {
  if (scale < 1) scale = 1;
  return n === 0 ? 0 : scale * ((fontW(font) + 1) * n - 1);
}
export const TEXT_HEIGHT = (scale, font = FONT_5X7) =>
  (font === FONT_16X24 ? 24 : 7) * scale;

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
    default:  return undefined;
  }
}

export function expandVars(input, secs) {
  const tm = tagTime(secs);
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
      out += String(n).padStart(width, zero ? '0' : ' ');
    } else if (name === 'W') {
      out += WDAY_NAME[tm.wday % 7];
    } else if (name === 'M') {
      out += MONTH_NAME[(tm.month - 1) % 12];
    } else if (name === 'P') {
      out += tm.hour < 12 ? 'AM' : 'PM';
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
export function runScript(panel, script, secs) {
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
        const shown = expandVars(str, secs);

        /* align= moves the anchor: x is the left edge at 0, the centre at 1,
         * the right edge at 2. Measured after expansion, because the width of
         * "{H:02d}:{N:02d}" is not the width of "09:41". Math.trunc to match
         * the firmware's integer division - both floor for the positive
         * widths this can produce, but the intent should not rest on that. */
        const align = a.named('align', 0);
        const font = a.named('font', FONT_5X7);
        let tx = x;
        if (align !== 0) {
          const w = textWidth(shown.length, scale, font);
          tx -= align === 1 ? Math.trunc(w / 2) : w;
        }

        /* The large font has digits and ':' only. The tag draws anything else
         * blank without complaint - it has nowhere to complain to - so this is
         * the only place an author finds out before looking at the panel. */
        if (font === FONT_16X24) {
          const missing = [...new Set(shown)].filter((c) => !FONT16[c]);
          if (missing.length) {
            warnings.push({
              line: n + 1, text: line,
              msg: `font=1 has no glyph for ${missing.map((c) => `'${c}'`).join(', ')}`
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
