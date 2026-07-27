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

import { Panel, runScript, expandVars, tagTime, tagSecondsNow, textWidth, evalArg,
         OPTIONS, EVERY_MAX }
  from './epd.js';
import { PRESETS } from './presets.js';
import { dither, toPanel, surface, DITHERS } from './image.js';
import { IMAGE_BYTES } from './ble.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const PARSER_C = join(HERE,
  '../firmware/hema_epd_clock/src/epd/epd_cmdparser.c');

/* Read the buffer limits from the firmware rather than repeating them. They
 * have moved once and would have gone stale here silently - a preset over the
 * old limit would still have passed, and one under a raised limit would have
 * been rejected for nothing. */
const CFILE = readFileSync(PARSER_C, 'utf8');
const cdef = (name) => {
  const m = new RegExp(`#define\\s+${name}\\s+(\\d+)`).exec(CFILE);
  assert.ok(m, `${name} not found in epd_cmdparser.c`);
  return Number(m[1]);
};
const SCRIPT_MAX = cdef('CMD_SCRIPT_MAX');
const LINE_MAX = cdef('CMD_LINE_MAX');

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
    assert.ok(bytes <= SCRIPT_MAX,
      `${name}: ${bytes} bytes exceeds CMD_SCRIPT_MAX (${SCRIPT_MAX})`);
    for (const line of script.split('\n')) {
      assert.ok(line.length < LINE_MAX, `${name}: a line exceeds CMD_LINE_MAX`);
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

test('indentation is formatting, not a syntax error', () => {
  /* skip_ws() in the firmware. A face reads better with its blocks indented,
   * and a line the tag silently drops is the worst thing to author against. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    'ROTATE(3)\n  CLEAR(1)\n\tRECT(4,4,40,20,fill=1)\n', SECS);
  assert.deepEqual(warnings, []);
  assert.equal(p.get(4, 4), 0, 'the indented RECT did not draw');
});

test('a wrong-case command says so instead of "not implemented"', () => {
  /* Commands are case-sensitive because {d} and {D} are, so the preview has to
   * refuse `rect(` exactly as the tag does - but a bare "not implemented" for
   * a command that plainly exists sends the author looking in the wrong place. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, 'ROTATE(3)\nrect(4,4,40,20,0,1,1)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /must be written RECT\(\).*case-sensitive/);
  assert.ok(p.fb.every((b) => b === 0xff), 'a rejected command still drew');
});

test('a space before the paren is not a command', () => {
  /* The firmware matches the literal prefix "RECT(", so "RECT (" is nothing. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, 'ROTATE(3)\nRECT (4,4,40,20,0,1,1)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.ok(p.fb.every((b) => b === 0xff));
});

test('an option is not swallowed as a positional', () => {
  /* The point of the whole redesign: under the old purely positional list,
   * leaving an argument out slid every later one into the wrong slot and the
   * face drew somewhere else with nothing to say why. Here the missing
   * positionals read 0 and the option is still found by name. */
  const p = new Panel();
  p.clear(1);
  runScript(p, 'ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=1)\n', SECS);
  assert.equal(p.get(30, 20), 0, 'the rect did not fill');
  assert.equal(p.get(70, 20), 1, 'the fill ran past the rect');
});

test('options may appear in any order and be left out', () => {
  const render = (script) => {
    const p = new Panel();
    p.clear(1);
    runScript(p, script, SECS);
    return Buffer.from(p.fb);
  };
  const a = render('ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,color=0,width=2,fill=0)\n');
  const b = render('ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,width=2)\n');
  const c = render('ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=0,width=2,color=0)\n');
  assert.deepEqual(a, b, 'omitting an option did not fall back to its default');
  assert.deepEqual(a, c, 'option order changed the result');
});

test("a '=' inside quoted text is text", () => {
  /* find_named() has to skip quoted regions, or a face that prints "scale=9"
   * would silently resize itself.
   *
   * The comma inside the quotes is the whole point and is not decoration: the
   * scan only looks for an option at the start of an argument, so a quoted
   * "scale=9" with no comma before it is never at one and passes even with the
   * quote handling removed. It takes a comma *inside* the string to push the
   * fake option to an argument boundary. Found by mutation - the first version
   * of this test passed against a deliberately broken findNamed(). */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    "ROTATE(3)\nCLEAR(1)\nFONT(2,2,'A,scale=9',scale=2)\n", SECS);
  assert.deepEqual(warnings, []);

  /* At scale=2 the 9 glyphs of "A,scale=9" are 2*(6*9-1) = 106 px wide, so
   * column 110 is clear. At scale=9 they would be 477 and cover it. */
  assert.equal(p.get(110, 2), 1, 'text rendered wider than scale=2 allows');
});

test('an option name must match whole, not by prefix', () => {
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    'ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,colors=1)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /no colors= option/);
  /* "colors" must not have satisfied the lookup for "color". */
  assert.equal(p.get(4, 4), 0, 'the outline vanished, so colors= was read as color=');
});

test('the option tables agree with the firmware', () => {
  /* The firmware has its own copy of which options each command reads, so it
   * can report an unknown one over the status characteristic. Two hand-written
   * tables of the same thing is exactly the pair that drifts the moment an
   * option is added on one side, and the drift is invisible: the preview would
   * accept an option the tag ignores, or warn about one it honours. */
  const c = readFileSync(PARSER_C, 'utf8');

  const lists = {};
  for (const m of c.matchAll(/OPTS_(\w+)\[\]\s*=\s*\{([^}]*)\}/g)) {
    lists[`OPTS_${m[1]}`] = [...m[2].matchAll(/"(\w+)"/g)].map((x) => x[1]);
  }
  assert.ok(Object.keys(lists).length >= 4, 'no OPTS_ tables found');

  /* Read the wiring, not just the tables: which list a command is actually
   * checked against is the thing that matters, and OPTS_SHAPE serves two. */
  const wired = {};
  for (const block of c.split('starts_with(line, "').slice(1)) {
    const cmd = block.slice(0, block.indexOf('('));
    const use = /check_options\(args,\s*(OPTS_\w+)\)/.exec(block.slice(0, 2000));
    if (use) wired[cmd] = lists[use[1]];
  }

  for (const [cmd, opts] of Object.entries(wired)) {
    assert.deepEqual(new Set(OPTIONS[cmd]), new Set(opts),
      `${cmd}(): epd.js and epd_cmdparser.c disagree about its options`);
  }
  for (const cmd of ['CLEAR', 'POINT', 'LINE', 'RECT', 'CIRCLE', 'FONT', 'ROTATE']) {
    assert.ok(cmd in wired, `${cmd}() is not checked for unknown options`);
  }
});

test('the firmware reports what it made of a script', () => {
  /* Checks the actual C, via `render --status`. The report is the tag's own
   * account of the render, so a preview warning that the firmware does not
   * agree with is worse than no warning at all. */
  const st = (script) => {
    const out = execFileSync(RENDER, [String(SECS), '--status'], {
      input: script, encoding: 'utf8',
    });
    const [fmt, code, lineLo, lineHi, count, flags, lenLo, lenHi,
           everyLo, everyHi] = out.trim().split(' ').map(Number);
    return { fmt, code, line: lineLo | (lineHi << 8), count, flags,
             len: lenLo | (lenHi << 8), every: everyLo | (everyHi << 8) };
  };

  const OK = 0, UNKNOWN_CMD = 1, UNKNOWN_OPT = 2;

  assert.deepEqual(st('CLEAR(1)\nRECT(1,1,9,9,fill=1)\n'),
    { fmt: 2, code: OK, line: 0, count: 0, flags: 0, len: 30, every: 1 });

  let s = st('CLEAR(1)\nICON(1,2,3)\nRECT(1,1,9,9)\n');
  assert.equal(s.code, UNKNOWN_CMD);
  assert.equal(s.line, 2, 'wrong line for the unknown command');

  s = st('CLEAR(1)\nRECT(1,1,9,9,nope=1)\n');
  assert.equal(s.code, UNKNOWN_OPT);
  assert.equal(s.line, 2);

  /* An option that exists, on a command that does not take it. */
  assert.equal(st('LINE(1,1,9,9,fill=1)\n').code, UNKNOWN_OPT);

  /* Only the first problem is located, but all of them are counted. */
  s = st('BOGUS(1)\nRECT(1,1,9,9,zzz=1)\n');
  assert.equal(s.code, UNKNOWN_CMD);
  assert.equal(s.line, 1);
  assert.equal(s.count, 2);

  /* The quoting rule again, from the firmware's own side this time. */
  assert.equal(st("FONT(2,2,'A,scale=9',scale=2)\n").code, OK);

  /* CRLF is one line ending. Counting it as two would put every reported line
   * number past the first one out by a line, in a file an editor shows as
   * perfectly ordinary. */
  assert.equal(st('CLEAR(1)\r\nICON(1)\r\n').line, 2);

  /* A line over CMD_LINE_MAX is reported as such, and specifically NOT as
   * "the script buffer is full" - both conditions used to set the same flag,
   * which would send an author shortening the whole face instead of the one
   * line. The line number is 0 because epd_cmd_feed() splits an over-long line
   * as it streams in, before there is a line number to report. */
  s = st(`CLEAR(1)\n${'RECT(' + '1,'.repeat(70)}9)\n`);
  assert.equal(s.code, 3, 'EPD_ERR_LINE_TOO_LONG');
  assert.equal(s.flags & 0x02, 0x02, 'the over-long-line flag');
  assert.equal(s.flags & 0x01, 0, 'must not also claim the script is full');

  /* A line of nothing but whitespace is a blank line. It used to be reported
   * as an unknown command, because skip_ws() strips indentation for matching
   * but the line still had length - so an editor leaving a couple of spaces on
   * a separating line put a phantom error in the report. The preview never
   * agreed: runScript() has always trimmed and skipped these. */
  assert.equal(st('CLEAR(1)\n   \nRECT(1,1,9,9)\n').code, OK,
    'a whitespace-only line must not be an unknown command');
  assert.equal(st('CLEAR(1)\n\t \nRECT(1,1,9,9)\n').count, 0);

  /* But indentation must not hide a real typo. */
  assert.equal(st('CLEAR(1)\n   NOPE(1)\n').code, UNKNOWN_CMD);
});

test('an unknown option is reported rather than silently dropped', () => {
  const p = new Panel();
  const { warnings } = runScript(p, 'CLEAR(1)\nRECT(1,1,9,9,nope=1)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /RECT\(\) has no nope= option.*color=, width=, fill=/);

  const { warnings: w2 } = runScript(new Panel(), 'CLEAR(1,scale=2)\n', SECS);
  assert.match(w2[0].msg, /CLEAR\(\) takes no options/);
});

test('an option value is a full expression', () => {
  const p = new Panel();
  p.clear(1);
  runScript(p, 'ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=(1+1)/2)\n', SECS);
  assert.equal(p.get(30, 20), 0, 'fill=(1+1)/2 did not evaluate to 1');
});

test('a bare CR ends a line, as epd_cmd_run() has it', () => {
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    'ROTATE(3)\rCLEAR(1)\rRECT(4,4,40,20,fill=1)\n', SECS);
  assert.deepEqual(warnings, []);
  assert.equal(p.get(4, 4), 0);
});

test("FONT's quoted text may contain commas", () => {
  /* The arg splitter has to respect quoting, or 'A,B' would be split into two
   * arguments and the text would silently truncate. */
  const p = new Panel();
  p.clear(1);
  runScript(p, "ROTATE(3)\nFONT(0,0,'A,B')\n", SECS);
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
    "ROTATE(1)\nCLEAR(0)\nFONT(2,2,'{W} {M} {j} {V} {G} {L}',color=1,bg=0)\n",
    "ROTATE(1)\nCLEAR(0)\nFONT(2,2,'{d}/{D} {j}/{J} {L}',color=1,bg=0)\n",
    "ROTATE(2)\nCLEAR(1)\nCIRCLE(60,60,40,width=2)\nRECT(5,5,50,30,fill=1)\n",

    /* Named arguments. The cursor walk and the per-option re-scan have to agree
     * about where an argument starts, so these poke at the seam between them. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=1,color=0,width=2)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,width=2,fill=1)\n",   /* order swapped */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30, fill = 1 , color = 0 )\n",  /* spaces */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=1+{d}%2,width={m}/4)\n", /* exprs */
    /* An option where a positional was expected: the positional reads 0 and
     * the option is still found, rather than being eaten as a coordinate. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,fill=1)\nCIRCLE(60,60,color=1)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(20,20,scale=4)\n",       /* text omitted */
    "ROTATE(3)\nCLEAR(1)\nFONT(20,20,'HI',scale=4,bg=0,color=1)\n",
    /* A '=' inside quoted text is text, not an option - and the comma inside
     * the quotes is load-bearing, since only an argument boundary can be
     * mistaken for the start of an option. See the unit test of the same name. */
    "ROTATE(3)\nCLEAR(1)\nFONT(2,2,'A,scale=9,fill=1',scale=2)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(2,2,'X,color=1',color=0,scale=2)\n",
    /* Prefix collision: neither "colors" nor "fills" is an option, so both
     * commands draw with their defaults. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,colors=1,fills=1)\n",
    /* Unknown options are ignored, not fatal - the tag has nowhere to say so. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,nope=7,fill=1)\n",
    /* A malformed option value is 0, like every other malformed argument. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,60,30,fill=,width=1/0)\n",
    "ROTATE(3)\nCLEAR(1)\nLINE(-20,-20,300,200,width=3)\nPOINT(249,121)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(0,0,'{h}{P} A,B',scale=4)\n",
    /* Off-panel and degenerate input: both sides must clip, not wrap. */
    "ROTATE(3)\nCLEAR(1)\nRECT(240,110,400,400,fill=1)\nFONT(230,0,'XYZ',scale=3)\n",

    /* INVERT. It is the only primitive that reads the framebuffer, so it is
     * the only one whose result depends on what was drawn first - which makes
     * it the likeliest to drift. Over glyphs, over a filled rect, and over the
     * seam between them. */
    "ROTATE(3)\nCLEAR(1)\nFONT(10,10,'27',scale=2)\nINVERT(8,8,24,20)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(0,0,100,40,fill=1)\nINVERT(20,10,40,20)\n",
    /* Twice over the same box is identity - if the two disagree about which
     * pixels are covered, this is where it shows. */
    "ROTATE(3)\nCLEAR(1)\nFONT(10,10,'88',scale=3)\nINVERT(5,5,50,30)\nINVERT(5,5,50,30)\n",
    /* Every rotation: a framebuffer byte is 8 pixels along the panel's x axis,
     * so under 1 and 3 the box crosses byte boundaries differently. */
    "ROTATE(0)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    "ROTATE(1)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    "ROTATE(2)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    /* Clipping, including a box entirely off-panel and one straddling the
     * edge, plus degenerate sizes that must draw nothing rather than wrap. */
    "ROTATE(3)\nCLEAR(1)\nINVERT(240,110,80,80)\nINVERT(-30,-30,50,50)\n",
    "ROTATE(3)\nCLEAR(1)\nINVERT(10,10,0,20)\nINVERT(10,40,20,0)\nINVERT(10,60,-5,-5)\n",
    /* Computed from expressions, the way a calendar highlighting today does. */
    "ROTATE(3)\nCLEAR(1)\nINVERT(8+({d}%7)*20,30+({d}/7)*14,19,13)\n",

    /* align=. The anchor shifts by a width measured after {} expansion, so
     * these poke at both the metric and the point it is taken. */
    "ROTATE(3)\nCLEAR(1)\nFONT(125,10,'CENTRED',align=1)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(245,10,'RIGHT',align=2)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(125,40,'{H:02d}:{N:02d}',scale=4,align=1)\n",
    /* Odd widths: w/2 truncates, and both sides must truncate the same way. */
    "ROTATE(3)\nCLEAR(1)\nFONT(125,10,'ABC',scale=3,align=1)\nFONT(125,40,'AB',scale=3,align=1)\n",
    /* Anchored off-panel, so the clip does the rest. */
    "ROTATE(3)\nCLEAR(1)\nFONT(0,10,'OFFLEFT',align=2)\nFONT(249,30,'OFFRIGHT',align=1)\n",
    /* Empty text must not shift anything by -scale. */
    "ROTATE(3)\nCLEAR(1)\nFONT(125,10,'',align=1)\nFONT(4,4,'X')\n",
    /* Unknown align values behave as "not centre" on both sides. */
    "ROTATE(3)\nCLEAR(1)\nFONT(125,10,'ODD',align=7)\n",

    /* font=1, the 16x24 digits. The two tables are generated from the same
     * ASCII art, so a drift here means one copy was hand-patched. */
    "ROTATE(3)\nCLEAR(1)\nFONT(4,4,'0123456789',font=1)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(20,40,'{H:02d}:{N:02d}',font=1,scale=2)\n",
    /* Missing glyphs draw blank on both sides rather than folding to 5x7. */
    "ROTATE(3)\nCLEAR(1)\nFONT(4,4,'AB:12',font=1)\n",
    /* align= over the wider cell - a shared width rule, two cell widths. */
    "ROTATE(3)\nCLEAR(1)\nFONT(125,40,'12:34',font=1,align=1)\n",
    "ROTATE(3)\nCLEAR(1)\nFONT(245,40,'12:34',font=1,align=2)\n",
    /* Clipping at the far edge, where the 24-row cell runs off the bottom. */
    "ROTATE(3)\nCLEAR(1)\nFONT(240,110,'88',font=1,scale=2)\n",
    /* An unknown font id falls back to 5x7 on both sides. */
    "ROTATE(3)\nCLEAR(1)\nFONT(4,4,'123',font=9)\n",

    /* EVERY draws nothing, but both sides must agree it is a known command -
     * if one of them warned or errored, the other's frame would still match. */
    "ROTATE(3)\nCLEAR(1)\nEVERY(60)\nFONT(4,4,'X')\n",

    /* Whitespace-only lines are blank lines, not mistyped commands. */
    "ROTATE(3)\nCLEAR(1)\n   \n\t\nFONT(4,4,'X')\n",

    /* Expression arguments. These are where the two implementations are most
     * likely to drift: JS numbers are doubles and do not wrap at 32 bits, its
     * '/' is not integer division, and division by zero is Infinity rather
     * than the firmware's deliberate 0. */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,4+{d}*8,12,fill=1)\nLINE(60,60,60+{H}*2,60,width=2)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,fill=1)\n",
    "ROTATE(3)\nCLEAR(1)\nCIRCLE(125-{N},61,{L}-{d}+8,0,2,1)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,4+{j}*241/{J},12,fill=1)\nPOINT({D},{J}%250)\n",
    "ROTATE(3)\nCLEAR(1)\nPOINT({j}%250,{V}*2)\nRECT(0,0,{u}%200,10,fill=1)\n",
    /* Malformed on purpose: both must yield 0 and carry on. */
    "ROTATE(3)\nCLEAR(1)\nRECT(1/0,{W},{nope},10%0,fill=1)\nPOINT(-(3+4),8)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT((((((((((1+1)))))))))*4,4,80,20,fill=1)\n",
    /* A missing comma. The firmware stops the first argument at the space and
     * resumes the next one at the 2, so it sees 1,2,3 - a preview that split
     * on commas would see 1,3,0 and draw somewhere else entirely. */
    "ROTATE(3)\nCLEAR(1)\nPOINT(1 2,3)\nRECT(8 9,4,40,20,fill=1)\n",
    /* Quoted text still wins over expression syntax inside it. */
    "ROTATE(3)\nCLEAR(1)\nFONT(2,2,'A,B (1+2)',scale=2)\n",

    /* Malformed *lines*, as opposed to malformed arguments above. Every one of
     * these disagreed before it was listed here: the JS trimmed and
     * upper-cased a line the firmware matched with a literal prefix, so an
     * indented or lower-cased command previewed as a shape the tag ignored,
     * and a lone CR previewed as one broken line where the tag ran two. The
     * lesson is that the well-formed scripts above cannot catch a divergence
     * in how a line is *recognised* - only deliberately ugly input can. */
    "ROTATE(3)\nCLEAR(1)\n  RECT(4,4,40,20,fill=1)\n",       /* indented */
    "ROTATE(3)\nCLEAR(1)\n\tRECT(4,4,40,20,fill=1)\n",       /* tab-indented */
    "ROTATE(3)\nCLEAR(1)\nrect(4,4,40,20,0,1,1)\n",         /* wrong case */
    "ROTATE(3)\nCLEAR(1)\nRect(4,4,40,20,0,1,1)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT (4,4,40,20,0,1,1)\n",        /* space before ( */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,40,20,fill=1)   \n",      /* trailing space */
    "ROTATE(3)\nCLEAR(1)\rRECT(4,4,40,20,fill=1)\n",         /* bare CR */
    "ROTATE(3)\r\nCLEAR(1)\r\nRECT(4,4,40,20,fill=1)\r\n",   /* CRLF */
    "ROTATE(3)\nCLEAR(1)\n# a note\nRECT(4,4,40,20,fill=1)\n",
    "ROTATE(3)\n\n\nCLEAR(1)\nRECT(4,4,40,20,fill=1)\n",     /* blank lines */
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,40,20,fill=1)",           /* no final \n */
    "ROTATE(3)\nCLEAR(1)\nRECT\nRECT(4,4,40,20,fill=1)\n",   /* no ( at all */
    /* A longer name that merely starts with a command's letters. Matching on
     * "RECT" rather than "RECT(" would draw a rectangle here. */
    "ROTATE(3)\nCLEAR(1)\nRECTANGLE(4,4,40,20,fill=1)\n",
    /* Too few and too many arguments: missing ones read as 0, extra ones are
     * never read. RECT() collapses to a single pixel at the origin under the
     * corner form - worth pinning, because it is exactly the case that becomes
     * "draw nothing" when RECT moves to x/y/w/h. */
    "ROTATE(3)\nCLEAR(1)\nRECT()\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,40,20,0,1,1,9,9,9)\n",
    "ROTATE(3)\nCLEAR(1)\nRECT(4,4,40,20,0,1,1\n",          /* unclosed */
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

test('EVERY() sets the repaint interval on both sides', { skip:
      existsSync(RENDER) ? false : 'run: make -C firmware/hema_epd_clock/test render'
    }, () => {
  /* The interval never reaches the framebuffer, so the frame-parity test above
   * cannot see it drift. Compare the two directly instead. */
  const cEvery = (script) => Number(execFileSync(RENDER, [String(SECS), '--every'], {
    input: script, encoding: 'utf8',
  }).trim());

  const jsEvery = (script) => {
    const p = new Panel();
    p.clear(1);
    return runScript(p, script, SECS).every;
  };

  const cases = [
    ['CLEAR(1)\n', 1, 'a face that says nothing repaints every minute'],
    ['CLEAR(1)\nEVERY(1)\n', 1],
    ['CLEAR(1)\nEVERY(15)\n', 15],
    ['CLEAR(1)\nEVERY(60)\n', 60],
    ['CLEAR(1)\nEVERY(1440)\n', 1440],
    /* Clamped, not rejected: a shelf label has nowhere to report a refusal,
     * and 0 would otherwise mean "never repaint again". */
    ['CLEAR(1)\nEVERY(0)\n', 1, 'zero clamps up'],
    ['CLEAR(1)\nEVERY(-5)\n', 1, 'negative clamps up'],
    ['CLEAR(1)\nEVERY(99999)\n', EVERY_MAX, 'clamps down to a day'],
    /* Last one wins, on both sides. */
    ['CLEAR(1)\nEVERY(60)\nEVERY(5)\n', 5],
    /* Expressions work here as anywhere else. */
    ['CLEAR(1)\nEVERY(2*30)\n', 60],
  ];

  for (const [script, want, why] of cases) {
    assert.equal(cEvery(script), want, `firmware: ${why || script.trim()}`);
    assert.equal(jsEvery(script), want, `preview: ${why || script.trim()}`);
  }
});

test('the repaint interval does not leak between scripts', { skip:
      existsSync(RENDER) ? false : 'run: make -C firmware/hema_epd_clock/test render'
    }, () => {
  /* Each run resets it, so dropping EVERY() from a face restores once a
   * minute. Left standing, a tag could sit on an hourly interval with no line
   * anywhere in its script saying so - and no way for its author to find out.
   *
   * The JS side runs both scripts through one module instance, which is where
   * a module-level `every` would show up. */
  const p = new Panel();
  p.clear(1);
  assert.equal(runScript(p, 'CLEAR(1)\nEVERY(1440)\n', SECS).every, 1440);
  assert.equal(runScript(p, 'CLEAR(1)\n', SECS).every, 1,
    'the previous script\'s interval leaked into the next');

  /* And the firmware, which is where it actually matters: the tag runs script
   * after script in one process. A "%%" line makes render do the same. */
  const after = (scripts) => Number(execFileSync(RENDER, [String(SECS), '--every'], {
    input: scripts.join('\n%%\n'), encoding: 'utf8',
  }).trim());

  assert.equal(after(['CLEAR(1)\nEVERY(1440)\n']), 1440, 'harness sanity');
  assert.equal(after(['CLEAR(1)\nEVERY(1440)\n', 'CLEAR(1)\n']), 1,
    'the firmware kept the previous face\'s interval');
  assert.equal(after(['CLEAR(1)\nEVERY(1440)\n', 'CLEAR(1)\nEVERY(30)\n']), 30);
});

const FONT_TOOL = join(HERE, '../tools/font16.py');

test('both copies of the 16x24 table match the generator', { skip:
      existsSync(FONT_TOOL) ? false : 'tools/font16.py is missing'
    }, () => {
  /* The table exists twice - once in the firmware, once here - because the
   * preview has to draw what the panel draws. Both are generated from the
   * ASCII art in tools/font16.py, and the only way that stays true is if
   * something checks. A hand-patched copy would show up as a preview that
   * disagrees with the tag about the shape of a digit, which is exactly the
   * class of bug the whole parity harness exists to prevent.
   *
   * Compares the emitted text against what is actually in each file, so this
   * fails on a stale copy as well as on an edited one. */
  const emit = (flag) =>
    execFileSync('python3', [FONT_TOOL, flag], { encoding: 'utf8' }).trim();

  const cSrc = readFileSync(join(HERE,
    '../firmware/hema_epd_clock/src/epd/epd_gfx.c'), 'utf8');
  const jsSrc = readFileSync(join(HERE, 'epd.js'), 'utf8');

  const cTable = /static const glyph16x24_t FONT_16X24\[\] = \{[\s\S]*?\n\};/
    .exec(cSrc);
  const jsTable = /const FONT16 = \{[\s\S]*?\n\};/.exec(jsSrc);
  assert.ok(cTable, 'FONT_16X24 not found in epd_gfx.c');
  assert.ok(jsTable, 'FONT16 not found in epd.js');

  assert.equal(cTable[0], emit('--emit'),
    'FONT_16X24 in epd_gfx.c has drifted from tools/font16.py');
  assert.equal(jsTable[0], emit('--js'),
    'FONT16 in epd.js has drifted from tools/font16.py');
});

test('the 16x24 font is a font, not the small one scaled up', () => {
  const ink = (script) => {
    const p = new Panel();
    p.setRotation(3);
    p.clear(1);
    runScript(p, script, SECS);
    let n = 0;
    for (let y = 0; y < p.height; y++)
      for (let x = 0; x < p.width; x++) if (!p.get(x, y)) n++;
    return n;
  };

  /* A 5x7 '8' at scale 1 cannot ink more than 35 pixels; the large one is
   * drawn from its own table and inks far more. */
  const small = ink("CLEAR(1)\nFONT(4,4,'8')\n");
  const big = ink("CLEAR(1)\nFONT(4,4,'8',font=1)\n");
  assert.ok(small <= 35, `5x7 '8' inked ${small}, more than its cell holds`);
  assert.ok(big > small * 3, `font=1 '8' inked ${big}, no bigger than 5x7`);

  /* Width follows the wider cell: ((16 + 1)n - 1) * scale. */
  assert.equal(textWidth(5, 1, 1), 84);
  assert.equal(textWidth(5, 2, 1), 168);
  assert.equal(textWidth(1, 1, 1), 16);
  assert.equal(textWidth(0, 1, 1), 0);

  /* Every digit and the colon is present. A mistyped table entry would leave
   * one character silently invisible, which on a clock is a wrong time. */
  for (const c of '0123456789:') {
    assert.ok(ink(`CLEAR(1)\nFONT(4,4,'${c}',font=1)\n`) > 0,
      `large glyph '${c}' is blank`);
  }

  /* A character the table lacks draws blank rather than folding to 5x7 - and
   * the preview says so, since the tag cannot. */
  assert.equal(ink("CLEAR(1)\nFONT(4,4,'A',font=1)\n"), 0);
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, "CLEAR(1)\nFONT(4,4,'A1',font=1)\n", SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /no glyph for 'A'/);
  assert.ok(!/'1'/.test(warnings[0].msg), 'digits are not missing');
});

test('align= anchors text rather than centring it on the screen', () => {
  /* Expected positions are derived from the metric here, not by re-running the
   * renderer, so a wrong metric cannot agree with itself. */
  const draw = (script) => {
    const p = new Panel();
    p.setRotation(3);
    p.clear(1);
    runScript(p, script, SECS);
    /* Leftmost and rightmost inked columns. */
    let lo = 1e9, hi = -1;
    for (let y = 0; y < p.height; y++)
      for (let x = 0; x < p.width; x++)
        if (!p.get(x, y)) { lo = Math.min(lo, x); hi = Math.max(hi, x); }
    return { lo, hi };
  };

  const w = textWidth(5, 2);                 /* 'HELLO' at scale 2 */
  assert.equal(w, 58);

  const left = draw("CLEAR(1)\nFONT(100,10,'HELLO',scale=2)\n");
  assert.equal(left.lo, 100, 'align=0 puts x at the left edge');

  const centre = draw("CLEAR(1)\nFONT(100,10,'HELLO',scale=2,align=1)\n");
  assert.equal(centre.lo, 100 - Math.trunc(w / 2), 'align=1 centres on x');

  const right = draw("CLEAR(1)\nFONT(100,10,'HELLO',scale=2,align=2)\n");
  assert.equal(right.lo, 100 - w, 'align=2 puts the right edge at x');
  assert.ok(right.hi < 100, 'align=2 must not draw past the anchor');

  /* Centring on the panel is align=1 at the panel's own centre - the point of
   * anchoring rather than screen-centring. */
  const onPanel = draw("CLEAR(1)\nFONT(125,10,'HELLO',scale=2,align=1)\n");
  const slack = 250 - w;
  assert.equal(onPanel.lo, 125 - Math.trunc(w / 2));
  assert.ok(Math.abs(onPanel.lo - Math.floor(slack / 2)) <= 1,
    'centring on x=125 should land within a pixel of a hand-centred face');
});

test('an empty string has no width', () => {
  /* (6n - 1) alone gives -scale for n = 0, which would shift an align=1 anchor
   * the wrong way by a whole scale unit rather than leaving it alone. */
  assert.equal(textWidth(0, 1), 0);
  assert.equal(textWidth(0, 6), 0);

  const p = new Panel();
  p.setRotation(3);
  p.clear(1);
  runScript(p, "CLEAR(1)\nFONT(125,10,'',align=1)\n", SECS);
  assert.ok(p.fb.every((b) => b === 0xff), 'empty text drew ink');
});

test('the month grid highlights today, and only today', () => {
  /* The expected cell is derived from Date here, not from the same expression
   * the preset uses - a test that reused the formula would agree with it even
   * when both were wrong.
   *
   * Column is the weekday; row is how many weeks in from the 1st. The box is
   * the 20x13 one INVERT() draws, offset 2px up and left of the glyph so it
   * frames the number rather than clipping it. */
  const grid = PRESETS['Month grid'];
  const noHighlight = grid.replace(/INVERT\(.*\n/, '');
  assert.notEqual(noHighlight, grid, 'the INVERT line was not found to strip');

  const dates = [
    [2026, 7, 27], [2026, 2, 14], [2026, 2, 1],
    [2026, 8, 31],                       /* last day, bottom row */
    [2024, 2, 29],                       /* leap day */
    [2026, 3, 1],                        /* 1st landing on a Sunday */
  ];

  for (const [y, m, d] of dates) {
    const secs = Math.floor(Date.UTC(y, m - 1, d, 9, 0, 0) / 1000) - 946684800;
    const render = (script) => {
      const p = new Panel();
      p.setRotation(0);
      p.clear(1);
      runScript(p, script, secs);
      return p;
    };
    const plain = render(noHighlight);
    const lit = render(grid);

    let n = 0, x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1;
    for (let yy = 0; yy < 122; yy++) {
      for (let xx = 0; xx < 250; xx++) {
        if (plain.get(xx, yy) !== lit.get(xx, yy)) {
          n++;
          x0 = Math.min(x0, xx); y0 = Math.min(y0, yy);
          x1 = Math.max(x1, xx); y1 = Math.max(y1, yy);
        }
      }
    }

    const wday = new Date(Date.UTC(y, m - 1, d)).getUTCDay();
    const firstCol = new Date(Date.UTC(y, m - 1, 1)).getUTCDay();
    const rowIdx = Math.floor((firstCol + d - 1) / 7);
    const where = `${y}-${m}-${d}`;

    assert.equal(n, 20 * 13, `${where}: highlight is not a 20x13 box`);
    assert.deepEqual([x0, x1], [6 + wday * 34, 6 + wday * 34 + 19],
      `${where}: highlight is in the wrong column`);
    assert.deepEqual([y0, y1], [30 + rowIdx * 14, 30 + rowIdx * 14 + 12],
      `${where}: highlight is in the wrong row`);
  }
});

test('the month grid stops at the end of the month', () => {
  /* Days 29-31 are pushed off-panel by n/({D}+1), since the DSL cannot skip a
   * line. A short month must therefore draw strictly less ink than a long one
   * - if the trick stopped working, February would show a 30th and a 31st. */
  const ink = (y, m, d) => {
    const secs = Math.floor(Date.UTC(y, m - 1, d, 9, 0, 0) / 1000) - 946684800;
    const p = new Panel();
    p.setRotation(0);
    p.clear(1);
    runScript(p, PRESETS['Month grid'], secs);
    let dark = 0;
    for (let yy = 0; yy < 122; yy++)
      for (let xx = 0; xx < 250; xx++) if (!p.get(xx, yy)) dark++;
    return dark;
  };

  /* Same weekday-of-the-1st is not required; what matters is the day count. */
  const feb28 = ink(2026, 2, 10);      /* 28 days */
  const feb29 = ink(2024, 2, 10);      /* 29 days, leap */
  const mar31 = ink(2026, 3, 10);      /* 31 days */

  assert.ok(feb28 < feb29, 'a leap February must show one more day than a common one');
  assert.ok(feb29 < mar31, 'February must show fewer days than March');
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
  runScript(p, 'ROTATE(3)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,fill=1)\n', SECS);

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
