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
         OPTIONS, EVERY_MAX, PANELS }
  from './epd.js';
import { PRESETS } from './faces_data.js';
import * as Store from './store.js';
import { filterFaces } from './gallery.js';
import { dither, toPanel, surface, DITHERS } from './image.js';
import { imageBytes, RENDER_ERRORS, CMD_SERVICE, CMD_CHAR,
         IMG_SERVICE, IMG_CHAR, STATUS_CHAR } from './ble.js';

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

/* Concatenate the C string literals of the one DEFAULT_FACE[] in `chunk`. */
function firmwareFace(chunk) {
  const block = chunk.split('static const char DEFAULT_FACE[] =')[1].split(';')[0];
  return [...block.matchAll(/"((?:[^"\\]|\\.)*)"/g)]
    .map((m) => m[1].replace(/\\n/g, '\n'))
    .join('');
}

test('the default preset matches the firmware byte for byte', () => {
  const c = readFileSync(PARSER_C, 'utf8');

  /* There are two faces now, one per panel, in a single #if/#else. Attribute
   * them by which arm they sit in rather than by order of appearance, so
   * swapping the arms around cannot quietly swap the assertions too. */
  const lo = c.indexOf('#if defined(EPD_PANEL_LOW_RES)');
  const mid = c.indexOf('#else', lo);
  const end = c.indexOf('#endif', mid);
  assert.ok(lo >= 0 && mid > lo && end > mid,
    'could not find the DEFAULT_FACE #if/#else in epd_cmdparser.c');

  for (const [key, chunk] of [['low', c.slice(lo, mid)], ['high', c.slice(mid, end)]]) {
    assert.equal(PRESETS[key]['Built-in default'], firmwareFace(chunk),
      `the ${key} built-in preset has drifted from DEFAULT_FACE[] in epd_cmdparser.c`);
  }
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
  assert.equal(textWidth('A', 1), 5);
  assert.equal(textWidth('09:41', 5), 145);
  assert.equal(textWidth('ABCDEFGHIJ', 2), 118);

  /* Takes the string, not a glyph count, because the 16x16 face mixes 8px
   * ASCII with 16px CJK and a count no longer determines a width. */
  assert.equal(textWidth('\u5e74', 1, 2), 16, 'one CJK cell');
  assert.equal(textWidth('8', 1, 2), 8, 'ASCII is half-width there');
  assert.equal(textWidth('2026\u5e74', 1, 2), 52, 'a date line as drawn');
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

/* Presets that are meant to be one flat colour - see webui/faces/. Named
 * rather than detected from the script, so adding a face that renders blank by
 * accident still fails rather than being taken for housekeeping. */
const SOLID_FACES = new Set(['White screen', 'Black screen']);

test('every preset renders cleanly and fits on the panel', () => {
  for (const [key, faces] of Object.entries(PRESETS)) {
  for (const [face, script] of Object.entries(faces)) {
    const name = `${key}/${face}`;
    const p = new Panel(PANELS[key]);
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

    if (SOLID_FACES.has(face)) {
      /* The maintenance screens are the exception the check above exists to
       * catch, so they are held to the opposite rule rather than waved
       * through: all of one colour or nothing, since a screen meant to wipe a
       * ghost off the panel that leaves a stray pixel is not doing its job. */
      assert.ok(ink === 0 || ink === total, `${name}: is not a solid fill`);
    } else {
      assert.ok(ink > total * 0.01, `${name}: renders (nearly) blank`);
      assert.ok(ink < total * 0.99, `${name}: renders (nearly) solid`);
    }
  }
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
    'ROTATE(270)\n  CLEAR(1)\n\tRECT(4,4,40,20,fill=1)\n', SECS);
  assert.deepEqual(warnings, []);
  assert.equal(p.get(4, 4), 0, 'the indented RECT did not draw');
});

test('a wrong-case command says so instead of "not implemented"', () => {
  /* Commands are case-sensitive because {d} and {D} are, so the preview has to
   * refuse `rect(` exactly as the tag does - but a bare "not implemented" for
   * a command that plainly exists sends the author looking in the wrong place. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, 'ROTATE(270)\nrect(4,4,40,20,0,1,1)\n', SECS);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0].msg, /must be written RECT\(\).*case-sensitive/);
  assert.ok(p.fb.every((b) => b === 0xff), 'a rejected command still drew');
});

test('a space before the paren is not a command', () => {
  /* The firmware matches the literal prefix "RECT(", so "RECT (" is nothing. */
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, 'ROTATE(270)\nRECT (4,4,40,20,0,1,1)\n', SECS);
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
  runScript(p, 'ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=1)\n', SECS);
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
  const a = render('ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,color=0,width=2,fill=0)\n');
  const b = render('ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,width=2)\n');
  const c = render('ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=0,width=2,color=0)\n');
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
    "ROTATE(270)\nCLEAR(1)\nTEXT(2,2,'A,scale=9',scale=2)\n", SECS);
  assert.deepEqual(warnings, []);

  /* At scale=2 the 9 glyphs of "A,scale=9" are 2*(6*9-1) = 106 px wide, so
   * column 110 is clear. At scale=9 they would be 477 and cover it. */
  assert.equal(p.get(110, 2), 1, 'text rendered wider than scale=2 allows');
});

test('an option name must match whole, not by prefix', () => {
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    'ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,colors=1)\n', SECS);
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
  for (const cmd of ['CLEAR', 'POINT', 'LINE', 'RECT', 'CIRCLE', 'TEXT', 'ROTATE']) {
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
  assert.equal(st("TEXT(2,2,'A,scale=9',scale=2)\n").code, OK);

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

  /* The renamed command is an unknown command to the tag - there is no alias,
   * deliberately, so a face still saying FONT() is reported rather than half
   * working. */
  assert.equal(st("CLEAR(1)\nFONT(4,4,'HI')\n").code, UNKNOWN_CMD);

  /* A ROTATE that is not a quarter turn is its own error, not "unknown
   * command" - the command exists and the argument is the problem, and an
   * author told the wrong one of those looks in the wrong place. */
  const BAD_ARG = 5;
  let r = st('ROTATE(3)\nCLEAR(1)\n');
  assert.equal(r.code, BAD_ARG, 'EPD_ERR_BAD_ARG');
  assert.equal(r.line, 1);
  for (const d of [0, 90, 180, 270]) {
    assert.equal(st(`ROTATE(${d})\nCLEAR(1)\n`).code, OK, `ROTATE(${d})`);
  }
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
  runScript(p, 'ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=(1+1)/2)\n', SECS);
  assert.equal(p.get(30, 20), 0, 'fill=(1+1)/2 did not evaluate to 1');
});

test('a bare CR ends a line, as epd_cmd_run() has it', () => {
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p,
    'ROTATE(270)\rCLEAR(1)\rRECT(4,4,40,20,fill=1)\n', SECS);
  assert.deepEqual(warnings, []);
  assert.equal(p.get(4, 4), 0);
});

test("TEXT's quoted text may contain commas", () => {
  /* The arg splitter has to respect quoting, or 'A,B' would be split into two
   * arguments and the text would silently truncate. */
  const p = new Panel();
  p.clear(1);
  runScript(p, "ROTATE(270)\nTEXT(0,0,'A,B')\n", SECS);
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
   * would simply hang the transfer forever - and one that produced too many
   * shears the picture, because the tag's stride is not this one.
   *
   * Checked per panel: the geometry is chosen at runtime now, and picking it
   * wrong is the failure this arithmetic exists to make impossible. */
  for (const geom of Object.values(PANELS)) {
    const { w, h, rot } = surface(true, geom);
    const p = toPanel(new Uint8Array(w * h), w, h, rot, geom);
    assert.equal(p.fb.length, imageBytes(geom), `${geom.key}: wrong buffer size`);
  }
  assert.equal(imageBytes(PANELS.high), 4000);
  assert.equal(imageBytes(PANELS.low), 2756);
});

test('both orientations cover the whole panel', () => {
  for (const geom of Object.values(PANELS)) {
    for (const landscape of [true, false]) {
      const { w, h } = surface(landscape, geom);
      assert.equal(w * h, geom.w * geom.h, `${geom.key}: orientation lost pixels`);
    }
  }
  assert.deepEqual(surface(true, PANELS.high), { w: 250, h: 122, rot: 3 });
  assert.deepEqual(surface(false, PANELS.high), { w: 122, h: 250, rot: 0 });
  assert.deepEqual(surface(true, PANELS.low), { w: 212, h: 104, rot: 3 });
  assert.deepEqual(surface(false, PANELS.low), { w: 104, h: 212, rot: 0 });
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

/* Pull one locale's row out of a `NAME[CMD_LOCALE_N][n]` table in the firmware
 * source. The tables are the one place the two languages' text is written out
 * by hand on each side, so scraping the C is what stops the preview inventing
 * names the panel will not show. */
function firmwareRow(table, locale, count) {
  const c = readFileSync(PARSER_C, 'utf8');
  const m = new RegExp(`${table}\\[CMD_LOCALE_N\\]\\[${count}\\] = \\{([\\s\\S]*?)\\n\\};`)
    .exec(c);
  assert.ok(m, `${table} not found in epd_cmdparser.c`);
  const rows = [...m[1].matchAll(/\{([^}]*)\}/g)]
    .map((r) => [...r[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]));
  assert.equal(rows.length, 3, `${table} should have three locales`);
  assert.equal(rows[locale].length, count);
  return rows[locale];
}

test('month names match the firmware table', () => {
  const names = firmwareRow('MONTH_NAME', 0, 12);
  for (let m = 1; m <= 12; m++) {
    assert.equal(expandVars('{M}', at(2026, m, 1)), names[m - 1]);
  }
});

test('the localised names match the firmware tables', () => {
  /* Both non-English rows of all three tables, against a script that selects
   * the locale - so this covers the LOCALE() command, the table contents and
   * the reset-per-run default in one place.
   *
   * Scraped rather than duplicated here: the strings are CJK, and a typo in a
   * character neither renderer would refuse is exactly the sort of thing that
   * reaches a panel unnoticed. */
  for (const locale of [1, 2]) {
    const wdays = firmwareRow('WDAY_NAME', locale, 7);
    const months = firmwareRow('MONTH_NAME', locale, 12);
    const ampm = firmwareRow('AMPM_NAME', locale, 2);

    for (let d = 0; d < 7; d++) {
      /* 2026-07-26 is a Sunday, so this walks the week from index 0. */
      const secs = at(2026, 7, 26 + d);
      assert.equal(expandVars('{W}', secs, { locale }), wdays[d]);
    }
    for (let m = 1; m <= 12; m++) {
      assert.equal(expandVars('{M}', at(2026, m, 1), { locale }), months[m - 1]);
    }
    assert.equal(expandVars('{P}', at(2026, 7, 26, 9), { locale }), ampm[0]);
    assert.equal(expandVars('{P}', at(2026, 7, 26, 15), { locale }), ampm[1]);
  }
});

test('LOCALE() selects the language and resets per run', () => {
  const p = new Panel();
  const secs = at(2026, 7, 26);          /* a Sunday */
  const shown = (script) => {
    const out = [];
    const orig = p.text.bind(p);
    p.text = (x, y, str, ...rest) => { out.push(str); return orig(x, y, str, ...rest); };
    runScript(p, script, secs);
    p.text = orig;
    return out;
  };

  assert.deepEqual(shown("CLEAR(1)\nTEXT(0,0,'{W}')\n"), ['SUN'],
    'English is the default');
  assert.deepEqual(shown("CLEAR(1)\nLOCALE(ja)\nTEXT(0,0,'{W}')\n"), ['日曜日']);
  assert.deepEqual(shown("CLEAR(1)\nLOCALE(zh)\nTEXT(0,0,'{W}')\n"), ['星期日']);
  /* Case folded, unlike command names - see lower() in epd_cmdparser.c for
   * why this one place is an exception. */
  assert.deepEqual(shown("CLEAR(1)\nLOCALE(JA)\nTEXT(0,0,'{W}')\n"), ['日曜日']);
  /* Mid-script, so it applies from where it appears rather than to the whole
   * face - the same way ROTATE does. */
  assert.deepEqual(
    shown("CLEAR(1)\nTEXT(0,0,'{W}')\nLOCALE(ja)\nTEXT(0,20,'{W}')\n"),
    ['SUN', '日曜日']);
  /* And it does not survive into the next run: a face that drops LOCALE()
   * must not inherit the previous one's language. */
  assert.deepEqual(shown("CLEAR(1)\nTEXT(0,0,'{W}')\n"), ['SUN']);

  const { warnings } = runScript(p, "CLEAR(1)\nLOCALE(xx)\n", secs);
  assert.equal(warnings.length, 1, 'an unknown code is reported');
  assert.match(warnings[0].msg, /not a language/);
});

test('the new variables pad like the old ones', () => {
  assert.equal(expandVars('{j:03d}', at(2026, 1, 5)), '005');
  assert.equal(expandVars('{V:02d}', at(2026, 1, 1)), '01');
  assert.equal(expandVars('{h:2d}', at(2026, 7, 26, 9)), ' 9');
  /* {V} must not swallow {VER}: the name is matched whole, not by prefix. */
  assert.equal(expandVars('{VER}', at(2026, 7, 26)), 'HEMA1');
});

test('the battery variables render only once a reading exists', () => {
  const t = (s, battPct, battMv) =>
    expandVars(s, at(2026, 7, 26), { battPct, battMv });

  /* No reading: both render literally, exactly as the firmware does before
   * anything has called epd_cmd_set_batt(). A face on a build that takes no
   * reading should say so rather than draw a confident 0%. */
  assert.equal(t('{BAT}%'), '{BAT}%');
  assert.equal(t('{VCC}mV'), '{VCC}mV');

  assert.equal(t('{BAT}%', 80, 2900), '80%');
  assert.equal(t('{VCC}mV', 80, 2900), '2900mV');
  assert.equal(t('{BAT:03d}', 80, 2900), '080', 'padding works on both');

  /* One supplied and not the other is a state the firmware cannot reach -
   * epd_cmd_set_batt() takes both - but the preview can, and each name has to
   * stand on its own rather than one gating the other. */
  assert.equal(t('{BAT}/{VCC}', 80, undefined), '80/{VCC}');

  /* The multi-letter names must not disturb the single-letter ones, and
   * {VER} must still reach the text branch rather than being read as a
   * number - the firmware's var_num() needs an explicit list to get this
   * right, so it is worth pinning on both sides. */
  assert.equal(t('{V}|{VER}|{VCC}', 80, 2900), '30|HEMA1|2900');
  assert.equal(t('{B}|{BAT}', 80, 2900), '{B}|80');
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
const RENDER_LOW = join(HERE, '../firmware/hema_epd_clock/test/render-low');

test('the JS renderer is byte-identical to the firmware C', { skip:
      existsSync(RENDER) && existsSync(RENDER_LOW) ? false
        : 'run: make -C firmware/hema_epd_clock/test render render-low'
    }, () => {
  /* Scripts aimed at the places the two could drift: clipping, odd rotations,
   * the calendar variables, quoting. Run against both panels below, along with
   * that panel's own presets - the clipping cases in particular land on a
   * different edge at 212x104 than at 250x122, which is free extra coverage. */
  const common = [
    "ROTATE(90)\nCLEAR(0)\nTEXT(2,2,'{W} {M} {j} {V} {G} {L}',color=1,bg=0)\n",
    "ROTATE(90)\nCLEAR(0)\nTEXT(2,2,'{d}/{D} {j}/{J} {L}',color=1,bg=0)\n",
    "ROTATE(180)\nCLEAR(1)\nCIRCLE(60,60,40,width=2)\nRECT(5,5,50,30,fill=1)\n",

    /* Named arguments. The cursor walk and the per-option re-scan have to agree
     * about where an argument starts, so these poke at the seam between them. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=1,color=0,width=2)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,width=2,fill=1)\n",   /* order swapped */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30, fill = 1 , color = 0 )\n",  /* spaces */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=1+{d}%2,width={m}/4)\n", /* exprs */
    /* An option where a positional was expected: the positional reads 0 and
     * the option is still found, rather than being eaten as a coordinate. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,fill=1)\nCIRCLE(60,60,color=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(20,20,scale=4)\n",       /* text omitted */
    "ROTATE(270)\nCLEAR(1)\nTEXT(20,20,'HI',scale=4,bg=0,color=1)\n",
    /* A '=' inside quoted text is text, not an option - and the comma inside
     * the quotes is load-bearing, since only an argument boundary can be
     * mistaken for the start of an option. See the unit test of the same name. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(2,2,'A,scale=9,fill=1',scale=2)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(2,2,'X,color=1',color=0,scale=2)\n",
    /* Prefix collision: neither "colors" nor "fills" is an option, so both
     * commands draw with their defaults. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,colors=1,fills=1)\n",
    /* Unknown options are ignored, not fatal - the tag has nowhere to say so. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,nope=7,fill=1)\n",
    /* A malformed option value is 0, like every other malformed argument. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,60,30,fill=,width=1/0)\n",
    "ROTATE(270)\nCLEAR(1)\nLINE(-20,-20,300,200,width=3)\nPOINT(249,121)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(0,0,'{h}{P} A,B',scale=4)\n",

    /* UTF-8 and the 16x16 font. Both sides have to decode the same bytes into
     * the same codepoints and then advance by the same per-glyph widths, so a
     * mixed-width string is the case that catches either half going wrong.
     * align= is in here because it multiplies any width disagreement by the
     * anchor rather than merely shifting the text one pixel. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'2026年8月10日',font=2)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,20,'星期一',font=2,align=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,20,'月曜日',font=2,align=2)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'12时34分',font=2,scale=2)\n",
    /* Lowercase and the degree sign: the 5x7 table used to have neither. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'Hello, world! 25°C',scale=2)\n",
    /* A character no font carries draws blank and still advances a cell. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'中文',font=2)\nTEXT(4,24,'A中B',font=2)\n",
    /* LOCALE(): the tables are written out by hand on each side, so the only
     * thing that proves they agree is rendering them. All three languages,
     * and one script that switches mid-face, which is where a preview that
     * treated the locale as face-wide would part company with the tag. */
    "ROTATE(270)\nCLEAR(1)\nLOCALE(ja)\nTEXT(4,4,'{W} {M}{d}日 {P}',font=2)\n",
    "ROTATE(270)\nCLEAR(1)\nLOCALE(zh)\nTEXT(4,4,'{W} {M}{d}日 {P}',font=2)\n",
    "ROTATE(270)\nCLEAR(1)\nLOCALE(en)\nTEXT(4,4,'{W} {M} {d} {P}')\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'{W}')\nLOCALE(ja)\nTEXT(4,24,'{W}',font=2)\n",
    /* An unknown code leaves the locale alone on both sides rather than
     * defaulting to something. */
    "ROTATE(270)\nCLEAR(1)\nLOCALE(ja)\nLOCALE(xx)\nTEXT(4,4,'{W}',font=2)\n",

    /* A two-byte sequence, so the decoder's 0xC0 branch is covered here and
     * not only its three-byte one.
     *
     * Malformed UTF-8 is deliberately absent: this harness passes the script
     * as a JS string and Node encodes it, so anything expressible here is
     * valid by construction - '\\x80' would arrive as the two bytes C2 80,
     * testing U+0080 rather than a stray continuation byte. The firmware's
     * resynchronisation is tested where raw bytes can actually be written,
     * in test_gfx.c. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'25°C ±1',scale=2)\n",
    /* Off-panel and degenerate input: both sides must clip, not wrap. */
    "ROTATE(270)\nCLEAR(1)\nRECT(240,110,400,400,fill=1)\nTEXT(230,0,'XYZ',scale=3)\n",

    /* INVERT. It is the only primitive that reads the framebuffer, so it is
     * the only one whose result depends on what was drawn first - which makes
     * it the likeliest to drift. Over glyphs, over a filled rect, and over the
     * seam between them. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(10,10,'27',scale=2)\nINVERT(8,8,24,20)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT(0,0,100,40,fill=1)\nINVERT(20,10,40,20)\n",
    /* Twice over the same box is identity - if the two disagree about which
     * pixels are covered, this is where it shows. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(10,10,'88',scale=3)\nINVERT(5,5,50,30)\nINVERT(5,5,50,30)\n",
    /* Every rotation: a framebuffer byte is 8 pixels along the panel's x axis,
     * so under 1 and 3 the box crosses byte boundaries differently. */
    "ROTATE(0)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    "ROTATE(90)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    "ROTATE(180)\nCLEAR(1)\nINVERT(3,3,17,29)\n",
    /* Clipping, including a box entirely off-panel and one straddling the
     * edge, plus degenerate sizes that must draw nothing rather than wrap. */
    "ROTATE(270)\nCLEAR(1)\nINVERT(240,110,80,80)\nINVERT(-30,-30,50,50)\n",
    "ROTATE(270)\nCLEAR(1)\nINVERT(10,10,0,20)\nINVERT(10,40,20,0)\nINVERT(10,60,-5,-5)\n",
    /* Computed from expressions, the way a calendar highlighting today does. */
    "ROTATE(270)\nCLEAR(1)\nINVERT(8+({d}%7)*20,30+({d}/7)*14,19,13)\n",

    /* align=. The anchor shifts by a width measured after {} expansion, so
     * these poke at both the metric and the point it is taken. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,10,'CENTRED',align=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(245,10,'RIGHT',align=2)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,40,'{H:02d}:{N:02d}',scale=4,align=1)\n",
    /* Odd widths: w/2 truncates, and both sides must truncate the same way. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,10,'ABC',scale=3,align=1)\nTEXT(125,40,'AB',scale=3,align=1)\n",
    /* Anchored off-panel, so the clip does the rest. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(0,10,'OFFLEFT',align=2)\nTEXT(249,30,'OFFRIGHT',align=1)\n",
    /* Empty text must not shift anything by -scale. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,10,'',align=1)\nTEXT(4,4,'X')\n",
    /* Unknown align values behave as "not centre" on both sides. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,10,'ODD',align=7)\n",

    /* font=1, the 16x24 digits. The two tables are generated from the same
     * ASCII art, so a drift here means one copy was hand-patched. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'0123456789',font=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(20,40,'{H:02d}:{N:02d}',font=1,scale=2)\n",
    /* Missing glyphs draw blank on both sides rather than folding to 5x7. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'AB:12',font=1)\n",
    /* align= over the wider cell - a shared width rule, two cell widths. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(125,40,'12:34',font=1,align=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(245,40,'12:34',font=1,align=2)\n",
    /* Clipping at the far edge, where the 24-row cell runs off the bottom. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(240,110,'88',font=1,scale=2)\n",
    /* An unknown font id falls back to 5x7 on both sides. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'123',font=9)\n",

    /* EVERY draws nothing, but both sides must agree it is a known command -
     * if one of them warned or errored, the other's frame would still match. */
    "ROTATE(270)\nCLEAR(1)\nEVERY(60)\nTEXT(4,4,'X')\n",

    /* Whitespace-only lines are blank lines, not mistyped commands. */
    "ROTATE(270)\nCLEAR(1)\n   \n\t\nTEXT(4,4,'X')\n",

    /* Expression arguments. These are where the two implementations are most
     * likely to drift: JS numbers are doubles and do not wrap at 32 bits, its
     * '/' is not integer division, and division by zero is Infinity rather
     * than the firmware's deliberate 0. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,4+{d}*8,12,fill=1)\nLINE(60,60,60+{H}*2,60,width=2)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,fill=1)\n",
    "ROTATE(270)\nCLEAR(1)\nCIRCLE(125-{N},61,{L}-{d}+8,0,2,1)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,4+{j}*241/{J},12,fill=1)\nPOINT({D},{J}%250)\n",
    "ROTATE(270)\nCLEAR(1)\nPOINT({j}%250,{V}*2)\nRECT(0,0,{u}%200,10,fill=1)\n",
    /* Malformed on purpose: both must yield 0 and carry on. */
    "ROTATE(270)\nCLEAR(1)\nRECT(1/0,{W},{nope},10%0,fill=1)\nPOINT(-(3+4),8)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT((((((((((1+1)))))))))*4,4,80,20,fill=1)\n",
    /* A missing comma. The firmware stops the first argument at the space and
     * resumes the next one at the 2, so it sees 1,2,3 - a preview that split
     * on commas would see 1,3,0 and draw somewhere else entirely. */
    "ROTATE(270)\nCLEAR(1)\nPOINT(1 2,3)\nRECT(8 9,4,40,20,fill=1)\n",
    /* Quoted text still wins over expression syntax inside it. */
    "ROTATE(270)\nCLEAR(1)\nTEXT(2,2,'A,B (1+2)',scale=2)\n",

    /* Malformed *lines*, as opposed to malformed arguments above. Every one of
     * these disagreed before it was listed here: the JS trimmed and
     * upper-cased a line the firmware matched with a literal prefix, so an
     * indented or lower-cased command previewed as a shape the tag ignored,
     * and a lone CR previewed as one broken line where the tag ran two. The
     * lesson is that the well-formed scripts above cannot catch a divergence
     * in how a line is *recognised* - only deliberately ugly input can. */
    "ROTATE(270)\nCLEAR(1)\n  RECT(4,4,40,20,fill=1)\n",       /* indented */
    "ROTATE(270)\nCLEAR(1)\n\tRECT(4,4,40,20,fill=1)\n",       /* tab-indented */
    "ROTATE(270)\nCLEAR(1)\nrect(4,4,40,20,0,1,1)\n",         /* wrong case */
    "ROTATE(270)\nCLEAR(1)\nRect(4,4,40,20,0,1,1)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT (4,4,40,20,0,1,1)\n",        /* space before ( */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,40,20,fill=1)   \n",      /* trailing space */
    "ROTATE(270)\nCLEAR(1)\rRECT(4,4,40,20,fill=1)\n",         /* bare CR */
    "ROTATE(270)\r\nCLEAR(1)\r\nRECT(4,4,40,20,fill=1)\r\n",   /* CRLF */
    "ROTATE(270)\nCLEAR(1)\n# a note\nRECT(4,4,40,20,fill=1)\n",
    "ROTATE(270)\n\n\nCLEAR(1)\nRECT(4,4,40,20,fill=1)\n",     /* blank lines */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,40,20,fill=1)",           /* no final \n */
    "ROTATE(270)\nCLEAR(1)\nRECT\nRECT(4,4,40,20,fill=1)\n",   /* no ( at all */
    /* A longer name that merely starts with a command's letters. Matching on
     * "RECT" rather than "RECT(" would draw a rectangle here. */
    "ROTATE(270)\nCLEAR(1)\nRECTANGLE(4,4,40,20,fill=1)\n",
    /* Too few and too many arguments: missing ones read as 0, extra ones are
     * never read. RECT() collapses to a single pixel at the origin under the
     * corner form - worth pinning, because it is exactly the case that becomes
     * "draw nothing" when RECT moves to x/y/w/h. */
    "ROTATE(270)\nCLEAR(1)\nRECT()\n",
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,40,20,0,1,1,9,9,9)\n",
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,40,20,0,1,1\n",          /* unclosed */
  ];

  /* Both awkward dates, not one: 2027-01-01 is where {V}/{G} disagree with
   * {y}, and 2024-02-29 is where {D} and {J} both take their leap value. A
   * parity bug in the calendar variables cannot hide behind an ordinary day
   * on either. */
  const dates = [
    Math.floor(Date.UTC(2027, 0, 1, 9, 5, 0) / 1000) - 946684800,
    Math.floor(Date.UTC(2024, 1, 29, 9, 5, 0) / 1000) - 946684800,
  ];

  for (const [key, bin] of [['high', RENDER], ['low', RENDER_LOW]]) {
    const geom = PANELS[key];
    const scripts = [...Object.values(PRESETS[key]), ...common];

    for (const secs of dates) {
      /* Three temperatures, not one. undefined is the build with no sensor,
       * where {T} must render literally on BOTH sides; 26 is the ordinary
       * case; -5 is the one that matters, because it is the only variable in
       * the language that can be negative and it broke zero-padding in both
       * renderers when it arrived - C printed 4294967291 and JS printed
       * "0-5" where printf gives "-05". */
      for (const [temp, extra] of [[undefined, []], [26, ['--temp', '26']],
                                   [-5, ['--temp', '-5']]]) {
      for (const script of scripts) {
        const c = execFileSync(bin, [String(secs), ...extra], {
          input: script, maxBuffer: 1 << 20,
        });

        const p = new Panel(geom);
        p.clear(1);
        runScript(p, script, secs, { temp });

        assert.equal(c.length, p.fb.length, `${key}: framebuffer sizes differ`);
        const at = c.findIndex((b, i) => b !== p.fb[i]);
        assert.equal(at, -1, at < 0 ? '' :
          `${key}: first difference at byte ${at} ` +
          `(native row ${(at / geom.wbytes) | 0}) at t=${secs} for:\n${script}`);
      }
      }
    }
  }
});

test('the battery variables render identically in C and JS', { skip:
      existsSync(RENDER) ? false : 'run: make -C firmware/hema_epd_clock/test render'
    }, () => {
  /* A separate test rather than another axis on the cross-product above,
   * which is already three temperatures by three dates by every preset.
   *
   * The unsupplied case is the one worth having: it is where the two can
   * disagree silently, because rendering "{BAT}" as text and rendering it as
   * a number are both plausible behaviours and only one of them is ours. */
  const secs = Math.floor(Date.UTC(2026, 6, 26, 9, 41, 0) / 1000) - 946684800;
  const scripts = [
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'{BAT}% {VCC}mV',scale=2)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'{BAT:03d}',scale=2)\n",
    /* As a numeric argument, not just as text: var_num() is shared with the
     * expression evaluator in the firmware, so a bar drawn from {BAT} has to
     * agree too. */
    "ROTATE(270)\nCLEAR(1)\nRECT(4,4,4+{BAT},12,fill=1)\n",
    "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'{V}|{VER}|{VCC}',scale=2)\n",
  ];

  for (const [pct, mv, extra] of [[undefined, undefined, []],
                                  [80, 2900, ['--batt', '80', '2900']],
                                  [0, 1980, ['--batt', '0', '1980']]]) {
    for (const script of scripts) {
      const c = execFileSync(RENDER, [String(secs), ...extra], {
        input: script, maxBuffer: 1 << 20,
      });

      const p = new Panel(PANELS.high);
      p.clear(1);
      runScript(p, script, secs, { battPct: pct, battMv: mv });

      const at = c.findIndex((b, i) => b !== p.fb[i]);
      assert.equal(at, -1, at < 0 ? '' :
        `first difference at byte ${at} with pct=${pct} mv=${mv} for:\n${script}`);
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

const FONT_TOOL = join(HERE, '../tools/genfont.py');

test('the generated font tables match their sources', { skip:
      existsSync(FONT_TOOL) ? false : 'tools/genfont.py is missing'
    }, () => {
  /* Every table exists twice - once in the firmware, once here - because the
   * preview has to draw what the panel draws. Both are generated from the
   * ASCII art in tools/font5.py and font16.py and the character list in
   * tools/glyphs.txt, and the only way that stays true is if something
   * checks. A hand-patched copy would show up as a preview that disagrees
   * with the tag about the shape of a character, which is exactly the class
   * of bug the whole parity harness exists to prevent.
   *
   * --check regenerates from the sources and compares against what is on
   * disk, so this fails on a stale copy as well as on an edited one. */
  execFileSync('python3', [FONT_TOOL, '--check'], { encoding: 'utf8' });
});

/* ------------------------------------------------------------------ */
/* store.js - what survives a reload                                   */
/* ------------------------------------------------------------------ */

/* A fresh in-memory store per test, so ordering between them cannot matter
 * and nothing here depends on a browser. */
function freshStore() {
  const m = new Map();
  Store.useStore({
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
  });
  return m;
}

test('a draft comes back only for the panel it was written on', () => {
  freshStore();
  Store.saveDraft('high', 'CLEAR(1)\n');

  assert.equal(Store.loadDraft('high').script, 'CLEAR(1)\n');
  /* The whole point: a 250 px layout restored into a 212 px session would put
   * the author in front of a face that does not fit and no note saying why. */
  assert.equal(Store.loadDraft('low'), null);

  Store.clearDraft();
  assert.equal(Store.loadDraft('high'), null);
});

test('saved faces are keyed by name and panel together', () => {
  freshStore();
  Store.saveFace('Kitchen', 'high', 'A');
  Store.saveFace('Kitchen', 'low', 'B');
  assert.equal(Store.listFaces().length, 2, 'same name, different panels');

  const again = Store.saveFace('Kitchen', 'high', 'C');
  assert.equal(again.replaced, true);
  assert.equal(Store.listFaces().length, 2);
  assert.equal(Store.listFaces().find((f) => f.panel === 'high').script, 'C');

  Store.deleteFace('Kitchen', 'high');
  assert.deepEqual(Store.listFaces().map((f) => f.panel), ['low']);
});

test('a face needs a name, and free names do not collide', () => {
  freshStore();
  assert.equal(Store.saveFace('   ', 'high', 'A').ok, false);

  assert.equal(Store.freeName('My face', 'high'), 'My face');
  Store.saveFace('My face', 'high', 'A');
  assert.equal(Store.freeName('My face', 'high'), 'My face 2');
  Store.saveFace('My face 2', 'high', 'A');
  assert.equal(Store.freeName('My face', 'high'), 'My face 3');
  /* Names are per panel, so the other one starts clean. */
  assert.equal(Store.freeName('My face', 'low'), 'My face');
});

test('export round-trips through import', () => {
  freshStore();
  Store.saveFace('One', 'high', 'A');
  Store.saveFace('Two', 'low', 'B');
  const bundle = Store.exportFaces();

  freshStore();
  const res = Store.importFaces(bundle);
  assert.equal(res.ok, true);
  assert.equal(res.added, 2);
  assert.equal(res.replaced, 0);
  assert.deepEqual(Store.listFaces().map((f) => f.name).sort(), ['One', 'Two']);

  /* Importing the same bundle again replaces rather than duplicating. */
  const twice = Store.importFaces(bundle);
  assert.equal(twice.replaced, 2);
  assert.equal(Store.listFaces().length, 2);
});

test('import refuses what it does not recognise', () => {
  freshStore();
  assert.equal(Store.importFaces('not json').ok, false);
  assert.equal(Store.importFaces('{"format":"something-else"}').ok, false);
  assert.equal(Store.importFaces(JSON.stringify(
    { format: 'hema-faces', version: 99, faces: [] })).ok, false);
  /* A bundle whose entries are all malformed is refused rather than silently
   * importing nothing and reporting success. */
  assert.equal(Store.importFaces(JSON.stringify(
    { format: 'hema-faces', version: 1, faces: [{ name: 'x' }] })).ok, false);
  assert.equal(Store.listFaces().length, 0);
});

test('storage that throws does not take the page with it', () => {
  Store.useStore({
    getItem() { throw new Error('private mode'); },
    setItem() { throw new Error('private mode'); },
    removeItem() { throw new Error('private mode'); },
  });
  /* Reads fall back, writes report failure, and nothing propagates - losing a
   * draft is survivable, an editor that will not load is not. */
  assert.deepEqual(Store.listFaces(), []);
  assert.equal(Store.loadDraft('high'), null);
  assert.equal(Store.saveDraft('high', 'A'), false);
  assert.equal(Store.saveFace('X', 'high', 'A').ok, false);
});

test('the gallery filter matches on name, case-insensitively', () => {
  const faces = [{ name: 'Big clock' }, { name: 'Calendar 中文' },
                 { name: 'Status' }];
  assert.deepEqual(filterFaces(faces, '').map((f) => f.name),
                   ['Big clock', 'Calendar 中文', 'Status'], 'empty shows all');
  assert.deepEqual(filterFaces(faces, 'CLOCK').map((f) => f.name), ['Big clock']);
  assert.deepEqual(filterFaces(faces, '  中文 ').map((f) => f.name),
                   ['Calendar 中文'], 'trimmed, and CJK matches');
  assert.deepEqual(filterFaces(faces, 'zzz'), []);
});

test('every element app.js reaches for exists in the page', () => {
  /* The gallery added a dozen new ids. A typo in one is a TypeError at load
   * with no test to catch it, because nothing else here touches the DOM - so
   * this checks the two files against each other statically.
   *
   * One direction only: ids built at runtime (`${id}Val`) and ids looked up
   * from a list would show up as false positives going the other way. */
  const app = readFileSync(join(HERE, 'app.js'), 'utf8');
  const html = readFileSync(join(HERE, 'index.html'), 'utf8');

  const used = new Set([...app.matchAll(/\$\('([^']+)'\)/g)].map((m) => m[1]));
  const have = new Set([...html.matchAll(/id="([^"]+)"/g)].map((m) => m[1]));

  const missing = [...used].filter((id) => !have.has(id));
  assert.deepEqual(missing, [],
    `app.js looks up ids the page does not define: ${missing.join(', ')}`);
});

const FACE_TOOL = join(HERE, '../tools/genfaces.py');

test('the generated face bundle matches webui/faces/', { skip:
      existsSync(FACE_TOOL) ? false : 'tools/genfaces.py is missing'
    }, () => {
  /* Same guard as the fonts above. faces_data.js is committed so the editor
   * works without anyone running a generator, which is exactly the situation
   * where a stale copy would go unnoticed - the page loads and shows the old
   * face. --check regenerates from webui/faces/ and compares. */
  execFileSync('python3', [FACE_TOOL, '--check'], { encoding: 'utf8' });
});

const STORE_C = join(HERE,
  '../firmware/hema_epd_clock/src/platform/epd_store.c');

/* Bump this together with EPD_STORE_VERSION in epd_store.c, and only when the
 * DSL changes in a way that makes an already-stored face render wrongly. */
const DSL_VERSION = 2;

const PARSER_H = join(HERE,
  '../firmware/hema_epd_clock/src/epd/epd_cmdparser.h');
const CUSTS_H = join(HERE,
  '../firmware/hema_epd_clock/src/custom_profile/user_custs1_def.h');

test('every error the firmware can report has a message here', () => {
  /* The tag sends a number; this table is the only thing that turns it into
   * words. A code added on the firmware side without an entry here shows up
   * to the author as "unknown problem code 5", which is worse than the
   * silence it replaced - it says something went wrong and refuses to say
   * what. EPD_ERR_BAD_ARG arrived exactly that way. */
  const h = readFileSync(PARSER_H, 'utf8');
  const body = /typedef enum \{([\s\S]*?)\} epd_err_t;/.exec(h);
  assert.ok(body, 'epd_err_t not found');

  const names = [...body[1].matchAll(/^\s*(EPD_ERR_\w+)/gm)].map((m) => m[1]);
  assert.ok(names.length >= 5, 'suspiciously few error codes parsed');

  assert.equal(RENDER_ERRORS.length, names.length,
    `epd_err_t has ${names.length} codes (${names.join(', ')}) but `
    + `RENDER_ERRORS describes ${RENDER_ERRORS.length}`);

  /* Index 0 is "nothing wrong" and is deliberately null; the rest must say
   * something. */
  assert.equal(RENDER_ERRORS[0], null);
  for (let i = 1; i < RENDER_ERRORS.length; i++) {
    assert.ok(typeof RENDER_ERRORS[i] === 'string' && RENDER_ERRORS[i].length,
      `${names[i]} has no message`);
  }
});

test('the UUIDs in ble.js match the firmware, byte order and all', () => {
  /* The C stores them little-endian, so the arrays read backwards relative to
   * the dashed strings - which makes a hand-transcription error easy and its
   * symptom obscure: the browser simply never finds the service, with nothing
   * to say whether the tag or the page is wrong. */
  const h = readFileSync(CUSTS_H, 'utf8');
  const uuidOf = (macro) => {
    const m = new RegExp(`#define\\s+${macro}\\s+\\{([^}]*)\\}`).exec(h);
    assert.ok(m, `${macro} not found`);
    const bytes = m[1].split(',').map((x) => parseInt(x.trim(), 16));
    assert.equal(bytes.length, 16, `${macro} is not 16 bytes`);
    const hex = bytes.reverse().map((b) => b.toString(16).padStart(2, '0')).join('');
    return [hex.slice(0, 8), hex.slice(8, 12), hex.slice(12, 16),
            hex.slice(16, 20), hex.slice(20)].join('-');
  };

  assert.equal(uuidOf('DEF_CMD_SVC_UUID_128'), CMD_SERVICE);
  assert.equal(uuidOf('DEF_CMD_CHAR_UUID_128'), CMD_CHAR);
  assert.equal(uuidOf('DEF_STATUS_CHAR_UUID_128'), STATUS_CHAR);
  assert.equal(uuidOf('DEF_IMG_SVC_UUID_128'), IMG_SERVICE);
  assert.equal(uuidOf('DEF_IMG_CHAR_UUID_128'), IMG_CHAR);

  /* And none of them is still the vendor's. */
  for (const u of [CMD_SERVICE, CMD_CHAR, IMG_SERVICE, IMG_CHAR, STATUS_CHAR]) {
    assert.ok(!/^0000[0-9a-f]{4}-0000-1000-8000-00805f9b34fb$/.test(u),
      `${u} is a Bluetooth-base UUID from the vendor's block`);
  }
});

test('the store version tracks breaking DSL changes', () => {
  /* A tag keeps its face in flash and restores it at boot, so a breaking
   * grammar change reaches the panel of a tag nobody is holding, with no host
   * in range to notice. EPD_STORE_VERSION is what turns that into a fallback
   * to the built-in face instead.
   *
   * Version 2 is FONT() -> TEXT() and ROTATE(270) for ROTATE(3). The second is
   * the dangerous one: a stored landscape face would come back portrait rather
   * than merely blank.
   *
   * This test cannot tell whether a change was breaking - only a person can.
   * What it can do is fail when the two numbers disagree, so the next breaking
   * change is a decision someone makes rather than one they forget. */
  const src = readFileSync(STORE_C, 'utf8');
  const m = /#define\s+EPD_STORE_VERSION\s+(\d+)u/.exec(src);
  assert.ok(m, 'EPD_STORE_VERSION not found in epd_store.c');
  assert.equal(Number(m[1]), DSL_VERSION,
    'epd_store.c and this test disagree about the DSL version - if the '
    + 'grammar just changed in a way that breaks stored faces, bump both; '
    + 'if it did not, bump neither');
});

test('the renamed and re-based commands say what changed', () => {
  /* FONT() and ROTATE(3) are the two things most likely to survive from a
   * script written against the old grammar, and both fail silently on the tag
   * - one draws nothing, the other leaves the panel in the wrong orientation.
   * The preview is the only place an author can be told, so it says the one
   * useful thing rather than "not implemented". */
  const warn = (script) => {
    const p = new Panel();
    p.clear(1);
    return runScript(p, script, SECS).warnings;
  };

  let w = warn("CLEAR(1)\nFONT(4,4,'HI')\n");
  assert.equal(w.length, 1);
  assert.match(w[0].msg, /FONT\(\) is now TEXT\(\)/);

  /* The index form: ROTATE(3) meant 270 and now means nothing. Saying so is
   * the difference between a one-word fix and a hunt. */
  w = warn('ROTATE(3)\nCLEAR(1)\n');
  assert.equal(w.length, 1);
  assert.match(w[0].msg, /not a quarter turn/);
  assert.match(w[0].msg, /used to mean 270 degrees/);

  /* A value that was never an index gets the plain message. */
  w = warn('ROTATE(45)\nCLEAR(1)\n');
  assert.equal(w.length, 1);
  assert.match(w[0].msg, /not a quarter turn/);
  assert.ok(!/used to mean/.test(w[0].msg));

  /* And the four that are valid stay silent. */
  for (const d of [0, 90, 180, 270]) {
    assert.deepEqual(warn(`ROTATE(${d})\nCLEAR(1)\n`), [],
      `ROTATE(${d}) should be accepted`);
  }
});

test('a bad ROTATE leaves the panel alone rather than guessing', () => {
  /* Not "snap to the nearest quarter turn": guessing would put the face
   * sideways and give the author nothing to go on. The rotation stays as it
   * was, which for a fresh script is portrait. */
  const p = new Panel();
  p.clear(1);
  runScript(p, 'ROTATE(90)\nROTATE(3)\nCLEAR(1)\n', SECS);
  assert.equal(p.width, 250, 'the refused ROTATE(3) changed the rotation');
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
  const small = ink("CLEAR(1)\nTEXT(4,4,'8')\n");
  const big = ink("CLEAR(1)\nTEXT(4,4,'8',font=1)\n");
  assert.ok(small <= 35, `5x7 '8' inked ${small}, more than its cell holds`);
  assert.ok(big > small * 3, `font=1 '8' inked ${big}, no bigger than 5x7`);

  /* Width follows the wider cell: ((16 + 1)n - 1) * scale. */
  assert.equal(textWidth('09:41', 1, 1), 84);
  assert.equal(textWidth('09:41', 2, 1), 168);
  assert.equal(textWidth('0', 1, 1), 16);
  assert.equal(textWidth('', 1, 1), 0);

  /* Every digit and the colon is present. A mistyped table entry would leave
   * one character silently invisible, which on a clock is a wrong time. */
  for (const c of '0123456789:') {
    assert.ok(ink(`CLEAR(1)\nTEXT(4,4,'${c}',font=1)\n`) > 0,
      `large glyph '${c}' is blank`);
  }

  /* A character the table lacks draws blank rather than folding to 5x7 - and
   * the preview says so, since the tag cannot. */
  assert.equal(ink("CLEAR(1)\nTEXT(4,4,'A',font=1)\n"), 0);
  const p = new Panel();
  p.clear(1);
  const { warnings } = runScript(p, "CLEAR(1)\nTEXT(4,4,'A1',font=1)\n", SECS);
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

  const w = textWidth('HELLO', 2);
  assert.equal(w, 58);

  const left = draw("CLEAR(1)\nTEXT(100,10,'HELLO',scale=2)\n");
  assert.equal(left.lo, 100, 'align=0 puts x at the left edge');

  const centre = draw("CLEAR(1)\nTEXT(100,10,'HELLO',scale=2,align=1)\n");
  assert.equal(centre.lo, 100 - Math.trunc(w / 2), 'align=1 centres on x');

  const right = draw("CLEAR(1)\nTEXT(100,10,'HELLO',scale=2,align=2)\n");
  assert.equal(right.lo, 100 - w, 'align=2 puts the right edge at x');
  assert.ok(right.hi < 100, 'align=2 must not draw past the anchor');

  /* Centring on the panel is align=1 at the panel's own centre - the point of
   * anchoring rather than screen-centring. */
  const onPanel = draw("CLEAR(1)\nTEXT(125,10,'HELLO',scale=2,align=1)\n");
  const slack = 250 - w;
  assert.equal(onPanel.lo, 125 - Math.trunc(w / 2));
  assert.ok(Math.abs(onPanel.lo - Math.floor(slack / 2)) <= 1,
    'centring on x=125 should land within a pixel of a hand-centred face');
});

test('an empty string has no width', () => {
  /* (6n - 1) alone gives -scale for n = 0, which would shift an align=1 anchor
   * the wrong way by a whole scale unit rather than leaving it alone. */
  assert.equal(textWidth('', 1), 0);
  assert.equal(textWidth('', 6), 0);

  const p = new Panel();
  p.setRotation(3);
  p.clear(1);
  runScript(p, "CLEAR(1)\nTEXT(125,10,'',align=1)\n", SECS);
  assert.ok(p.fb.every((b) => b === 0xff), 'empty text drew ink');
});

test('the month grid highlights today, and only today', () => {
  /* The expected cell is derived from Date here, not from the same expression
   * the preset uses - a test that reused the formula would agree with it even
   * when both were wrong.
   *
   * Column is the weekday; row is how many weeks in from the 1st. The box is
   * the 20x13 one INVERT() draws, sitting 4px left and 3px above the glyph at
   * (8 + col*34, 42 + row*14) so it frames the number rather than clipping it.
   *
   * High-res only, deliberately: the numbers below are that face's 34x14 cells,
   * not a property of the calendar arithmetic. The low-res grid is checked for
   * frame-parity against the C by the byte-identity test instead. */
  const grid = PRESETS.high['Month grid'];
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
    assert.deepEqual([x0, x1], [4 + wday * 34, 4 + wday * 34 + 19],
      `${where}: highlight is in the wrong column`);
    assert.deepEqual([y0, y1], [39 + rowIdx * 14, 39 + rowIdx * 14 + 12],
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
    runScript(p, PRESETS.high['Month grid'], secs);
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
  runScript(p, 'ROTATE(270)\nCLEAR(1)\nRECT((1+1)*2,4,(10+10)*2,20,fill=1)\n', SECS);

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
    runScript(p, PRESETS.high['Month progress'], secs);
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
