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

/* Native panel geometry - portrait, as the SSD1680 sees it. */
export const EPD_W = 122;
export const EPD_H = 250;
const WBYTES = (EPD_W + 7) >> 3;

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

const WDAY_NAME = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];

/* The firmware's clock counts seconds from 2000-01-01; Date works in Unix
 * seconds. The tag has no notion of a timezone, so local wall-clock time is
 * sent as if it were UTC and read back the same way - see tagSecondsNow(). */
export const EPOCH_2000 = 946684800;

/** Seconds since 2000-01-01 for the browser's *local* wall clock. */
export function tagSecondsNow(now = new Date()) {
  return Math.floor(now.getTime() / 1000) - now.getTimezoneOffset() * 60
         - EPOCH_2000;
}

/** Break tag-seconds into the fields the {} variables expose. */
export function tagTime(secs) {
  const d = new Date((secs + EPOCH_2000) * 1000);
  return {
    year: d.getUTCFullYear(), month: d.getUTCMonth() + 1, day: d.getUTCDate(),
    hour: d.getUTCHours(), min: d.getUTCMinutes(), sec: d.getUTCSeconds(),
    wday: d.getUTCDay(), u: secs,
  };
}

/* ------------------------------------------------------------------ */
/* Framebuffer + primitives - the epd_gfx.c port.                      */
/* ------------------------------------------------------------------ */

export class Panel {
  constructor() {
    this.fb = new Uint8Array(WBYTES * EPD_H);
    this.rot = 0;
    this.clear(0);
  }

  get width()  { return (this.rot & 1) ? EPD_H : EPD_W; }
  get height() { return (this.rot & 1) ? EPD_W : EPD_H; }

  setRotation(r) { this.rot = r & 3; }

  /* Rotated (drawing) coords -> native panel coords. Mirrors fb_set(). */
  _map(x, y) {
    switch (this.rot) {
      case 1:  return [EPD_W - 1 - y, x];
      case 2:  return [EPD_W - 1 - x, EPD_H - 1 - y];
      case 3:  return [y, EPD_H - 1 - x];
      default: return [x, y];
    }
  }

  set(x, y, color) {
    /* Clip in the rotated frame, before the transform, exactly as the firmware
     * does - so a shape clips against what the author can actually see. */
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
    const [px, py] = this._map(x, y);
    const idx = py * WBYTES + (px >> 3);
    const mask = 0x80 >> (px & 7);
    if (color) this.fb[idx] |= mask;      /* 1 = white */
    else       this.fb[idx] &= ~mask;     /* 0 = black */
  }

  get(x, y) {
    const [px, py] = this._map(x, y);
    const idx = py * WBYTES + (px >> 3);
    return (this.fb[idx] & (0x80 >> (px & 7))) ? 1 : 0;
  }

  clear(color) { this.fb.fill(color ? 0xff : 0x00); }

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

  text(x, y, str, fore, back, scale) {
    if (scale < 1) scale = 1;
    let cursor = x;
    for (const ch of str) {
      const glyph = FONT[ch.toUpperCase()] || null;
      for (let col = 0; col < 5; col++) {
        const bits = glyph ? glyph[col] : 0x00;
        for (let row = 0; row < 7; row++) {
          const color = ((bits >> row) & 1) ? fore : back;
          for (let sx = 0; sx < scale; sx++)
            for (let sy = 0; sy < scale; sy++)
              this.set(cursor + col * scale + sx, y + row * scale + sy, color);
        }
      }
      cursor += 6 * scale;              /* 5px glyph + 1px gap */
    }
  }
}

/** Rendered width of `n` glyphs at `scale` - no gap after the last one.
 *  Handy for centring text, which is most of what face authoring is. */
export function textWidth(n, scale) { return scale * (6 * n - 1); }
export const TEXT_HEIGHT = (scale) => 7 * scale;

/* ------------------------------------------------------------------ */
/* {} variable expansion - the expand_vars() port.                     */
/* ------------------------------------------------------------------ */

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
    const nums = {
      y: tm.year, m: tm.month, d: tm.day, H: tm.hour,
      N: tm.min, S: tm.sec, w: tm.wday, u: tm.u,
    };

    if (name in nums) {
      out += String(nums[name]).padStart(width, zero ? '0' : ' ');
    } else if (name === 'W') {
      out += WDAY_NAME[tm.wday % 7];
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

/* Split a command's argument list, respecting 'quoted strings' (which may
 * legitimately contain commas). Returns raw arg text, unparsed. */
function splitArgs(s) {
  const args = [];
  let cur = '', quoted = false;

  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (quoted) {
      if (c === "'") quoted = false; else cur += c;
    } else if (c === "'") {
      quoted = true;
    } else if (c === ',') {
      args.push(cur.trim()); cur = '';
    } else if (c === ')') {
      break;
    } else {
      cur += c;
    }
  }
  args.push(cur.trim());
  return args;
}

const num = (a, i) => {
  const v = parseInt(a[i], 10);
  return Number.isNaN(v) ? 0 : v;       /* parse_int() yields 0 on garbage */
};

/* Which commands take a quoted string, and where. */
const STRING_ARG = { FONT: 7 };

/**
 * Run a script against a panel.
 * Returns { warnings: [{line, text, msg}] } - authoring aid only; the firmware
 * itself ignores everything it doesn't recognise.
 */
export function runScript(panel, script, secs) {
  const warnings = [];
  const lines = script.split('\n');

  lines.forEach((raw, n) => {
    const line = raw.trim();
    if (!line || line.startsWith('#')) return;

    const open = line.indexOf('(');
    if (open < 0) {
      warnings.push({ line: n + 1, text: line, msg: 'not a command' });
      return;
    }
    const cmd = line.slice(0, open).trim().toUpperCase();
    const a = splitArgs(line.slice(open + 1));

    switch (cmd) {
      case 'CLEAR':
        panel.clear(num(a, 0));
        break;

      case 'POINT':
        panel.set(num(a, 0), num(a, 1), num(a, 2));
        break;

      case 'LINE':
        panel.line(num(a, 0), num(a, 1), num(a, 2), num(a, 3),
                   num(a, 4), num(a, 5));
        break;

      case 'RECT':
        panel.rect(num(a, 0), num(a, 1), num(a, 2), num(a, 3),
                   num(a, 4), num(a, 5), num(a, 6));
        break;

      case 'CIRCLE':
        panel.circle(num(a, 0), num(a, 1), num(a, 2),
                     num(a, 3), num(a, 4), num(a, 5));
        break;

      case 'FONT': {
        /* FONT(x, y, gap, font_id, fore, back, scale, 'text').
         * gap and font_id are accepted and ignored, as in the firmware -
         * the fallback font is fixed-pitch and there is only one of it. */
        const text = a[STRING_ARG.FONT] ?? '';
        panel.text(num(a, 0), num(a, 1), expandVars(text, secs),
                   num(a, 4), num(a, 5), num(a, 6));
        break;
      }

      case 'ROTATE': {
        let r = num(a, 0);
        /* The vendor's doc defines degrees and indices in the same breath, so
         * both are accepted. */
        if (r === 90) r = 1; else if (r === 180) r = 2; else if (r === 270) r = 3;
        panel.setRotation(r);
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
          msg: `${cmd}() is not implemented - the tag will ignore it`,
        });
    }
  });

  return { warnings };
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
