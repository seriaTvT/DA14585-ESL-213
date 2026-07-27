/*
 * test.mjs - node --test webui/test.mjs
 *
 * Guards the two things that quietly rot: the preview drifting away from the
 * firmware's renderer, and a preset that no longer fits on the panel. Neither
 * shows up as an error at runtime - the firmware clips silently and the
 * preview would just be confidently wrong - so they are checked here instead.
 *
 * No DOM: epd.js only touches `document` inside paint(), which is not exercised.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { Panel, runScript, expandVars, tagTime, tagSecondsNow, textWidth, evalArg }
  from './epd.js';
import { PRESETS } from './presets.js';
import { dither, toPanel, surface, DITHERS } from './image.js';
import { IMAGE_BYTES } from './ble.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const PARSER_C = join(HERE,
  '../firmware/hema_epd_clock/src/epd/epd_cmdparser.c');

/* A fixed instant, so the tests do not depend on when they run:
 * 2026-07-26 14:37:05, a Sunday. */
const SECS = Math.floor(Date.UTC(2026, 6, 26, 14, 37, 5) / 1000) - 946684800;

test('the default preset matches the firmware byte for byte', () => {
  const c = readFileSync(PARSER_C, 'utf8');
  const block = c.split('static const char DEFAULT_FACE[] =')[1].split(';')[0];
  const firmware = [...block.matchAll(/"((?:[^"\\]|\\.)*)"/g)]
    .map((m) => m[1].replace(/\\n/g, '\n'))
    .join('');

  assert.equal(PRESETS['Built-in default'], firmware,
    'the built-in preset has drifted from DEFAULT_FACE[] in epd_cmdparser.c');
});

test('{} expansion matches expand_vars()', () => {
  const t = (s) => expandVars(s, SECS);
  assert.equal(t('{H:02d}:{N:02d}:{S:02d}'), '14:37:05');
  assert.equal(t('{y}-{m:02d}-{d:02d}'), '2026-07-26');
  assert.equal(t('{W} {w}'), 'SUN 0');
  assert.equal(t('{VER}'), 'HEMA1');
  assert.equal(t('{u}'), String(SECS));
  assert.equal(t('{H:3d}'), ' 14', 'width without 0 pads with spaces');
  assert.equal(t('{q}'), '{q}', 'unknown names pass through literally');
  assert.equal(t('{'), '{', 'a bare brace is emitted literally');
  assert.equal(t('100~C'), '100~C', "'~' is the degree stand-in, not a variable");
});

test('the epoch and weekday agree with epd_time.c', () => {
  const tm = tagTime(0);
  assert.deepEqual(
    [tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec],
    [2000, 1, 1, 0, 0, 0], 'tag second 0 is 2000-01-01T00:00:00');
  assert.equal(tm.wday, 6, '2000-01-01 was a Saturday');
});

test('local wall-clock time is what gets sent', () => {
  /* The tag has no timezone, so the browser sends local time as if it were
   * UTC - a round trip through tagTime() must give back the local fields. */
  const now = new Date();
  const tm = tagTime(tagSecondsNow(now));
  assert.equal(tm.hour, now.getHours());
  assert.equal(tm.min, now.getMinutes());
  assert.equal(tm.day, now.getDate());
});

test('text metrics match epd_gfx_text()', () => {
  /* 5px glyph + 1px gap, no gap after the last glyph. */
  assert.equal(textWidth(1, 1), 5);
  assert.equal(textWidth(5, 5), 145);
  assert.equal(textWidth(10, 2), 118);
});

test('rotation transposes the frame', () => {
  const p = new Panel();
  for (const [rot, w, h] of [[0, 122, 250], [1, 250, 122],
                             [2, 122, 250], [3, 250, 122]]) {
    p.setRotation(rot);
    assert.equal(p.width, w, `rot ${rot} width`);
    assert.equal(p.height, h, `rot ${rot} height`);
  }
});

test('drawing clips instead of wrapping', () => {
  /* fb_set() bounds-checks in the rotated frame. A pixel one past the right
   * edge must vanish, not reappear on the next row - which is exactly what an
   * unchecked index into a row-major buffer would do. */
  const p = new Panel();
  p.setRotation(3);
  p.clear(1);
  p.set(p.width, 10, 0);
  p.set(-1, 10, 0);
  let dark = 0;
  for (let y = 0; y < p.height; y++)
    for (let x = 0; x < p.width; x++) if (!p.get(x, y)) dark++;
  assert.equal(dark, 0, 'out-of-bounds pixels leaked into the framebuffer');
});

test('every preset renders cleanly and fits on the panel', () => {
  for (const [name, script] of Object.entries(PRESETS)) {
    const p = new Panel();
    p.setRotation(0);
    p.clear(1);
    const { warnings } = runScript(p, script, SECS);

    assert.deepEqual(warnings, [], `${name}: unimplemented commands`);

    const bytes = Buffer.byteLength(script);
    assert.ok(bytes <= 1024, `${name}: ${bytes} bytes exceeds CMD_SCRIPT_MAX`);
    for (const line of script.split('\n')) {
      assert.ok(line.length < 128, `${name}: a line exceeds CMD_LINE_MAX`);
    }

    /* Ink somewhere, but not everywhere: an all-blank face means the geometry
     * put the text off-screen, which is the failure a preset is most likely to
     * have and the one a human is least likely to notice in a diff. */
    let ink = 0;
    for (let y = 0; y < p.height; y++)
      for (let x = 0; x < p.width; x++) if (!p.get(x, y)) ink++;
    const total = p.width * p.height;
    assert.ok(ink > total * 0.01, `${name}: renders (nearly) blank`);
    assert.ok(ink < total * 0.99, `${name}: renders (nearly) solid`);
  }
});

test('control commands draw nothing', () => {
  /* TIME() and RESET() are applied on arrival by the firmware and never
   * stored, so the preview must not treat them as content - and must not
   * report them as unimplemented either. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, 'TIME(12345)\nRESET()\n', SECS);
  assert.deepEqual(warnings, []);
  assert.ok(p.fb.every((b) => b === 0xff), 'a control command drew ink');
});

test('unimplemented commands are reported, not drawn', () => {
  const p = new Panel();
  const { warnings } = runScript(p, 'CLEAR(1)\nICON(1,2,3)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /ICON\(\) is not implemented/);
  assert.equal(warnings[0].line, 2);
});

test("FONT's quoted text may contain commas", () => {
  /* The arg splitter has to respect quoting, or 'A,B' would be split into two
   * arguments and the text would silently truncate. */
  const p = new Panel();
  p.clear(1);
  runScript(p, "ROTATE(3)\nFONT(0,0,0,0,0,1,1,'A,B')\n", SECS);
  let ink = 0;
  for (let y = 0; y < 7; y++)
    for (let x = 0; x < 18; x++) if (!p.get(x, y)) ink++;
  assert.ok(ink > 0, 'nothing drawn');
});

/* ------------------------------------------------------------------ */
/* Image pipeline                                                      */
/*                                                                     */
/* rasterize() needs a canvas and is not exercised here; everything     */
/* after it - dithering and packing - is pure and is where a mistake    */
/* would be invisible in the preview but wrong on the panel.            */
/* ------------------------------------------------------------------ */

const flat = (w, h, v) => new Float32Array(w * h).fill(v);

test('the image framebuffer is exactly what the tag waits for', () => {
  /* The protocol has no header and no length: the tag refreshes on the
   * EPD_BUF_SIZE'th byte, so a packing bug that produced one byte too few
   * would simply hang the transfer forever. */
  const { w, h, rot } = surface(true);
  const p = toPanel(new Uint8Array(w * h), w, h, rot);
  assert.equal(p.fb.length, IMAGE_BYTES);
  assert.equal(IMAGE_BYTES, 4000);
});

test('both orientations cover the whole panel', () => {
  for (const landscape of [true, false]) {
    const { w, h } = surface(landscape);
    assert.equal(w * h, 122 * 250);
  }
  assert.deepEqual(surface(true), { w: 250, h: 122, rot: 3 });
  assert.deepEqual(surface(false), { w: 122, h: 250, rot: 0 });
});

test('every dither maps flat black and flat white to solid output', () => {
  /* An off-by-one in a threshold comparison usually survives on a photo and
   * only shows up as speckle in what should be clean paper or clean ink. */
  for (const mode of Object.keys(DITHERS)) {
    const white = dither(flat(40, 40, 255), 40, 40, { dither: mode });
    const black = dither(flat(40, 40, 0), 40, 40, { dither: mode });
    assert.ok(white.every((v) => v === 1), `${mode} speckled a white field`);
    assert.ok(black.every((v) => v === 0), `${mode} speckled a black field`);
  }
});

test('error diffusion keeps the average tone', () => {
  /* The point of dithering: a flat 40% grey has no 40% to draw, so it has to
   * come out as roughly 40% of the pixels lit instead. */
  for (const mode of ['floyd-steinberg', 'atkinson']) {
    const bits = dither(flat(64, 64, 0.4 * 255), 64, 64, { dither: mode });
    const lit = bits.reduce((a, b) => a + b, 0) / bits.length;
    assert.ok(Math.abs(lit - 0.4) < 0.06,
      `${mode} rendered 40% grey as ${(lit * 100).toFixed(1)}% white`);
  }
});

test('ordered dither screens a flat mid grey instead of blanking it', () => {
  /* Bayer is the one mode that must *not* collapse mid grey to a solid, and
   * the threshold control has to still bias it. */
  const bits = dither(flat(64, 64, 128), 64, 64, { dither: 'ordered' });
  const lit = bits.reduce((a, b) => a + b, 0) / bits.length;
  assert.ok(lit > 0.3 && lit < 0.7, `mid grey came out ${lit}`);

  const dark = dither(flat(64, 64, 128), 64, 64,
                      { dither: 'ordered', threshold: 200 });
  assert.ok(dark.reduce((a, b) => a + b, 0) / dark.length < lit);
});

test('the threshold control splits where it says it does', () => {
  const opts = { dither: 'threshold', threshold: 100 };
  assert.equal(dither(flat(4, 4, 99), 4, 4, opts)[0], 0);
  assert.equal(dither(flat(4, 4, 100), 4, 4, opts)[0], 1);
});

test('packing survives the rotation transform', () => {
  /* toPanel() goes through Panel.set() so the rotation is the firmware's own.
   * This checks a landscape image comes back out of the panel unrotated -
   * i.e. that the write and the read agree, corners included. */
  const { w, h, rot } = surface(true);
  const bits = new Uint8Array(w * h).fill(1);
  const marks = [[0, 0], [w - 1, 0], [0, h - 1], [w - 1, h - 1], [7, 3]];
  for (const [x, y] of marks) bits[y * w + x] = 0;

  const p = toPanel(bits, w, h, rot);
  for (const [x, y] of marks) {
    assert.equal(p.get(x, y), 0, `mark at ${x},${y} moved or vanished`);
  }
  let ink = 0;
  for (const b of p.fb) ink += 8 - popcount(b);
  assert.equal(ink, marks.length, 'stray pixels outside the marks');
});

function popcount(b) {
  let n = 0;
  for (let i = 0; i < 8; i++) if (b & (1 << i)) n++;
  return n;
}

/* ------------------------------------------------------------------ */
/* Calendar variables                                                  */
/* ------------------------------------------------------------------ */

/** Tag-seconds for a UTC midnight, as the tag would hold it. */
const at = (y, m, d, hh = 0, mm = 0) =>
  Math.floor(Date.UTC(y, m - 1, d, hh, mm) / 1000) - 946684800;

test('ISO week numbering matches the C implementation and GNU date', () => {
  /* Every one of these is a case a naive (yday / 7) would get wrong: weeks
   * belonging to the neighbouring year, 53-week years, and leap days.
   * Expected values are `date -d <day> +%V/%G/%j`. */
  const cases = [
    ['2026-01-01', 1, 2026, 1],    // Thu - week 1 starts on new year's day
    ['2026-12-31', 53, 2026, 365], // 2026 is a 53-week year
    ['2027-01-01', 53, 2026, 1],   // Fri - still last year's week
    ['2021-01-01', 53, 2020, 1],
    ['2020-12-31', 53, 2020, 366], // leap year, 366 days
    ['2019-12-30', 1, 2020, 364],  // Mon - already next year's week 1
    ['2024-02-29', 9, 2024, 60],   // leap day
    ['2026-07-26', 30, 2026, 207],
    ['2000-01-01', 52, 1999, 1],   // the tag's own epoch
    ['2026-12-28', 53, 2026, 362],
  ];

  for (const [iso, week, wyear, yday] of cases) {
    const [y, m, d] = iso.split('-').map(Number);
    const tm = tagTime(at(y, m, d));
    assert.equal(tm.week, week, `${iso} week`);
    assert.equal(tm.wyear, wyear, `${iso} week-year`);
    assert.equal(tm.yday, yday, `${iso} day-of-year`);
  }
});

test('days in month tracks leap years', () => {
  assert.equal(tagTime(at(2024, 2, 1)).mdays, 29);
  assert.equal(tagTime(at(2026, 2, 1)).mdays, 28);
  assert.equal(tagTime(at(2000, 2, 1)).mdays, 29);  // divisible by 400
  assert.equal(tagTime(at(2100, 2, 1)).mdays, 28);  // divisible by 100, not 400
  for (const [m, n] of [[1, 31], [4, 30], [7, 31], [9, 30], [12, 31]]) {
    assert.equal(tagTime(at(2026, m, 1)).mdays, n, `month ${m}`);
  }
});

test('days in year tracks leap years, on the same rule as the month', () => {
  /* Same century-vs-400 cases as above, so {J} cannot pass by hardcoding 365
   * while {D} keeps the real leap test. */
  assert.equal(tagTime(at(2024, 6, 1)).ydays, 366);
  assert.equal(tagTime(at(2026, 6, 1)).ydays, 365);
  assert.equal(tagTime(at(2000, 6, 1)).ydays, 366);  // divisible by 400
  assert.equal(tagTime(at(2100, 6, 1)).ydays, 365);  // divisible by 100, not 400

  /* {J} is the bound {j} actually reaches - the pairing is only worth having
   * if the last day of the year lands exactly on it. */
  for (const y of [2024, 2026, 2000, 2100]) {
    const tm = tagTime(at(y, 12, 31));
    assert.equal(tm.yday, tm.ydays, `${y}-12-31 is the last day of the year`);
  }
});

test('lower case is the position, upper case the length', () => {
  /* The scheme the DSL documents: {d} of {D}, {j} of {J}. A leap February is
   * the one date where getting either backwards is visible. */
  assert.equal(expandVars('{d}/{D} {j}/{J}', at(2024, 2, 29)), '29/29 60/366');
  assert.equal(expandVars('{d}/{D} {j}/{J}', at(2026, 2, 28)), '28/28 59/365');
});

test('{L} still means what {D} means', () => {
  /* Undocumented, deliberately kept: a face stored on a tag before the rename
   * must not start rendering the literal "{L}" after a reflash. */
  for (const [y, m] of [[2024, 2], [2026, 2], [2026, 7], [2026, 4]]) {
    assert.equal(expandVars('{L}', at(y, m, 1)), expandVars('{D}', at(y, m, 1)),
      `${y}-${m}`);
  }
  const tm = tagTime(at(2026, 7, 26));
  assert.equal(evalArg('4+{d}*241/{L}', tm), evalArg('4+{d}*241/{D}', tm),
    'the alias resolves in expressions too, not just in text');
});

test('the 12-hour clock never shows hour zero', () => {
  /* The bug this guards is {h} rendering midnight as 0 and noon as 0. */
  const h = (hh) => expandVars('{h}{P}', at(2026, 7, 26, hh));
  assert.equal(h(0), '12AM');
  assert.equal(h(1), '1AM');
  assert.equal(h(11), '11AM');
  assert.equal(h(12), '12PM');
  assert.equal(h(13), '1PM');
  assert.equal(h(23), '11PM');
});

test('month names match the firmware table', () => {
  /* Same guard as the weekday table: the preview must not invent names the
   * panel will not show. */
  const c = readFileSync(PARSER_C, 'utf8');
  const table = /MONTH_NAME\[12\] = \{([^}]*)\}/.exec(c);
  assert.ok(table, 'MONTH_NAME not found in epd_cmdparser.c');
  const names = [...table[1].matchAll(/"(\w+)"/g)].map((m) => m[1]);
  assert.equal(names.length, 12);

  for (let m = 1; m <= 12; m++) {
    assert.equal(expandVars('{M}', at(2026, m, 1)), names[m - 1]);
  }
});

test('the new variables pad like the old ones', () => {
  assert.equal(expandVars('{j:03d}', at(2026, 1, 5)), '005');
  assert.equal(expandVars('{V:02d}', at(2026, 1, 1)), '01');
  assert.equal(expandVars('{h:2d}', at(2026, 7, 26, 9)), ' 9');
  /* {V} must not swallow {VER}: the name is matched whole, not by prefix. */
  assert.equal(expandVars('{VER}', at(2026, 7, 26)), 'HEMA1');
});

/* ------------------------------------------------------------------ */
/* Cross-language parity against the real firmware                     */
/*                                                                     */
/* Every other test in this file checks the JS port against a *reading* */
/* of the C. This one checks it against the C itself, compiled and run. */
/* Needs firmware/hema_epd_clock/test/render to be built:              */
/*     make -C firmware/hema_epd_clock/test render                     */
/* Skipped rather than failed when it is absent, so the suite still     */
/* runs on a machine with no compiler.                                  */
/* ------------------------------------------------------------------ */

const RENDER = join(HERE, '../firmware/hema_epd_clock/test/render');

test('the JS renderer is byte-identical to the firmware C', { skip:
      existsSync(RENDER) ? false : 'run: make -C firmware/hema_epd_clock/test render'
    }, () => {
  /* Every preset, plus scripts aimed at the places the two could drift:
   * clipping, odd rotations, the new calendar variables, quoting. */
  const scripts = [
    ...Object.values(PRESETS),
    "ROTATE(1)\nCLEAR(0)\nFONT(2,2,0,0,1,0,1,'{W} {M} {j} {V} {G} {L}')\n",
    "ROTATE(1)\nCLEAR(0)\nFONT(2,2,0,0,1,0,1,'{d}/{D} {j}/{J} {L}')\n",
    "ROTATE(2)\nCLEAR(1)\nCIRCLE(60,60,40,0,2,0)\nRECT(5,5,50,30,0,1,1)\n",
    "ROTATE(3)\nCLEAR(1)\nLINE(-20,-20,300,200,0,3)\nPOINT(249,121,0,1)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(0,0,0,0,0,1,4,'{h}{P} A,B')\n",
    /* Off-panel and degenerate input: both sides must clip, not wrap. */
    "ROTATE(3)\nCLEAR(1)\nRECT(240,110,400,400,0,1,1)\nFONT(230,0,0,0,0,1,3,'XYZ')\n",

    /* Expression arguments. These are where the two implementations are most
     * likely to drift: JS numbers are doubles and do not wrap at 32 bits, its
     * '/' is not integer division, and division by zero is Infinity rather
     * than the firmware's deliberate 0. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,4+{d}*8,12,0,1,1)\nLINE(60,60,60+{H}*2,60,0,2)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,0,1,1)\n",
    "ROTATE(3)\nCLEAR(1)\nCIRCLE(125-{N},61,{L}-{d}+8,0,2,1)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,4+{j}*241/{J},12,0,1,1)\nPOINT({D},{J}%250,0,1)\n",
    "ROTATE(3)\nCLEAR(1)\nPOINT({j}%250,{V}*2,0,1)\nRECT(0,0,{u}%200,10,0,1,1)\n",
    /* Malformed on purpose: both must yield 0 and carry on. */
    "ROTATE(3)\nCLEAR(1)\nRECT(1/0,{W},{nope},10%0,0,1,1)\nPOINT(-(3+4),8,0,1)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT((((((((((1+1)))))))))*4,4,80,20,0,1,1)\n",
    /* A missing comma. The firmware stops the first argument at the space and
     * resumes the next one at the 2, so it sees 1,2,3 - a preview that split
     * on commas would see 1,3,0 and draw somewhere else entirely. */
    "ROTATE(3)\nCLEAR(1)\nPOINT(1 2,3,0,1)\nRECT(8 9,4,40,20,0,1,1)\n",
    /* Quoted text still wins over expression syntax inside it. */
    "ROTATE(3)\nCLEAR(1)\nFONT(2,2,0,0,0,1,2,'A,B (1+2)')\n",
  ];

  /* Both awkward dates, not one: 2027-01-01 is where {V}/{G} disagree with
   * {y}, and 2024-02-29 is where {D} and {J} both take their leap value. A
   * parity bug in the calendar variables cannot hide behind an ordinary day
   * on either. */
  const dates = [
    Math.floor(Date.UTC(2027, 0, 1, 9, 5, 0) / 1000) - 946684800,
    Math.floor(Date.UTC(2024, 1, 29, 9, 5, 0) / 1000) - 946684800,
  ];

  for (const secs of dates) {
    for (const script of scripts) {
      const c = execFileSync(RENDER, [String(secs)], {
        input: script, maxBuffer: 1 << 20,
      });

      const p = new Panel();
      p.clear(1);
      runScript(p, script, secs);

      assert.equal(c.length, p.fb.length);
      const at = c.findIndex((b, i) => b !== p.fb[i]);
      assert.equal(at, -1, at < 0 ? '' :
        `first difference at byte ${at} (native row ${(at / 16) | 0}) ` +
        `at t=${secs} for:\n${script}`);
    }
  }
});

/* ------------------------------------------------------------------ */
/* Expression arguments                                                */
/* ------------------------------------------------------------------ */

test('numeric arguments evaluate expressions', () => {
  const tm = tagTime(at(2026, 7, 26, 14, 37));   /* Sunday, day 207 */
  const e = (s) => evalArg(s, tm);

  assert.equal(e('42'), 42);
  assert.equal(e('-7'), -7);
  assert.equal(e('2+3*4'), 14, 'precedence');
  assert.equal(e('(2+3)*4'), 20, 'parentheses');
  assert.equal(e('{d}'), 26);
  assert.equal(e('4+{d}*8'), 212);
  assert.equal(e('{L}-{d}'), 5, 'days left in the month');
  assert.equal(e(' 1 + 2 '), 3, 'spaces are skipped as in parse_int()');
  assert.equal(e('10%3'), 1);
  assert.equal(e('-(3+4)'), -7);
  assert.equal(e('--5'), 5, 'unary minus nests');
});

test('a bad expression yields 0 rather than throwing', () => {
  /* The firmware has nowhere to report an error to, so it renders something
   * wrong instead of hanging. The preview must agree, or the two diverge on
   * exactly the inputs where agreement matters most. */
  const tm = tagTime(at(2026, 7, 26));
  const e = (s) => evalArg(s, tm);

  assert.equal(e(''), 0);
  assert.equal(e(undefined), 0, 'a missing argument');
  assert.equal(e('1/0'), 0, 'division by zero is 0, not a trap');
  assert.equal(e('1%0'), 0);
  assert.equal(e('{W}'), 0, 'a text-valued variable has no number');
  assert.equal(e('{nope}'), 0, 'an unknown variable');
  assert.equal(e('{unterminated'), 0);
  assert.equal(e('+'), 0);
  assert.equal(e('()'), 0);
});

test('deep parenthesis nesting is capped, not recursed', () => {
  /* The firmware caps at EXPR_MAX_DEPTH to stay off a 1.7 KB stack. The exact
   * value past the cap matters less than both sides agreeing on it, which the
   * firmware-parity test checks; this pins that it terminates at all. */
  const tm = tagTime(at(2026, 7, 26));
  assert.equal(evalArg('('.repeat(200) + '1' + ')'.repeat(200), tm), 0);
  assert.equal(evalArg('((((1+1))))', tm), 2, 'ordinary nesting still works');
});

test('an argument list is split on top-level commas only', () => {
  /* A ')' inside an expression must not end the argument list. Getting this
   * wrong truncates arguments in the preview that the panel renders fine. */
  const p = new Panel();
  p.clear(1);
  runScript(p, 'ROTATE(3)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,0,1,1)\n', SECS);

  /* The rect spans x 4..40, y 4..20 - check a corner is inked and that the
   * area beyond where a truncated parse would have stopped is inked too. */
  assert.equal(p.get(4, 4), 0, 'rect did not start where the expression says');
  assert.equal(p.get(40, 20), 0, 'rect was truncated at the inner paren');
});

test('the month bar is symmetric and scales to the month', () => {
  /* Two separate mistakes are guarded here, both of which shipped once:
   * an outline computed as 4+{L}*8 overran the 250 px frame (last column 249)
   * so its right edge was clipped away entirely, and it also made the bar
   * itself a different size in February than in July. */
  const MARGIN = 4;
  const RIGHT = 249 - MARGIN;
  const row = (secs, y) => {
    const p = new Panel();
    p.clear(1);
    runScript(p, PRESETS['Month progress'], secs);
    p.setRotation(3);
    return p;
  };

  for (const [y, m, d, mdays] of [[2026, 7, 1, 31], [2026, 7, 26, 31],
                                  [2026, 7, 31, 31], [2026, 2, 15, 28],
                                  [2026, 2, 28, 28]]) {
    const p = row(at(y, m, d, 14, 37));

    /* The outline: first and last ink on its top edge. */
    let first = -1, last = -1;
    for (let x = 0; x < 250; x++) {
      if (!p.get(x, 70)) { if (first < 0) first = x; last = x; }
    }
    assert.equal(first, MARGIN, `${y}-${m}-${d}: left margin`);
    assert.equal(last, RIGHT, `${y}-${m}-${d}: right edge missing or clipped`);

    /* The fill: the rightmost pixel of the contiguous run from the left edge.
     * RECT is inclusive of both endpoints, so this is the x2 the expression
     * evaluated to, not a width. */
    let x = MARGIN;
    while (x < 250 && !p.get(x, 76)) x++;
    const fillRight = x - 1;
    assert.equal(fillRight, MARGIN + Math.trunc(d * (RIGHT - MARGIN) / mdays),
      `${y}-${m}-${d}: fill edge`);

    if (d === mdays) {
      assert.equal(fillRight, RIGHT,
        'the last day of the month should fill the bar exactly');
    }
  }
});
