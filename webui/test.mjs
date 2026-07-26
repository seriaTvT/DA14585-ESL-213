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
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { Panel, runScript, expandVars, tagTime, tagSecondsNow, textWidth }
  from './epd.js';
import { PRESETS } from './presets.js';

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
