/*
 * presets.js - starting-point faces.
 *
 * Kept out of app.js so they can be rendered headlessly by a test, which is
 * how the "byte-identical to the firmware's default" claim below stays true
 * rather than merely intended.
 */
import { textWidth } from './epd.js';

/* Landscape geometry, so x centring is against a 250 px wide frame. Text is
 * scale*(6n-1) wide for n glyphs - see textWidth(). Math.floor, not round, to
 * match the firmware's integer division. */
const centre = (n, scale) => Math.floor((250 - textWidth(n, scale)) / 2);

/* ---- month grid -----------------------------------------------------------
 * Built by a loop rather than written out, because the 31 lines differ only in
 * one number and a hand-maintained block of them would drift.
 *
 * The DSL has no conditionals and no way to bind an intermediate value, so
 * every line re-derives the same thing: where day 1 sits. FIRST_COL is the
 * weekday of the 1st - ({w}-{d}+71) mod 7, where +71 keeps the subtraction
 * positive and 71 = 1 (mod 7), so it lands on day 1 without disturbing the
 * modulus. That repetition is what makes this face ~2.1 KB; it is the reason
 * the script buffer was raised to 3072. */
const FIRST_COL = '(({w}-{d}+71)%7)';

/* Column of day n. Derived from n directly rather than from FIRST_COL, which
 * is the shorter of the two spellings here and worth it 31 times over. */
const dayCol = (n) => `(({w}-{d}+${n}+70)%7)`;
const dayRow = (n) => `((${FIRST_COL}+${n - 1})/7)`;

function monthGrid() {
  let s = '';
  for (let n = 1; n <= 31; n++) {
    /* Days 29-31 do not exist in every month, and there is no way to skip a
     * line. n/({D}+1) is 0 while the day is real and 1 once it is past the
     * end of the month, so the number is simply pushed off the panel and
     * clipped - February stops at 28 without a conditional. */
    const off = n >= 29 ? `+(${n}/({D}+1))*200` : '';
    s += `FONT(8+${dayCol(n)}*34,32+${dayRow(n)}*14${off},'${n}')\n`;
  }
  return s;
}

export const PRESETS = {
  /* Byte-identical to DEFAULT_FACE[] in epd_cmdparser.c, so "what the tag
   * ships with" is always one click away - and, since a test diffs the two,
   * stays that way if either side is edited. */
  'Built-in default':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(5, 5)},25,'{H:02d}:{N:02d}',scale=5)\n` +
    `FONT(${centre(10, 2)},72,'{y}-{m:02d}-{d:02d}',scale=2)\n` +
    `FONT(${centre(3, 2)},94,'{W}',scale=2)\n`,

  'Big clock':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(5, 7)},22,'{H:02d}:{N:02d}',scale=7)\n` +
    `FONT(${centre(14, 2)},88,'{W} {y}-{m:02d}-{d:02d}',scale=2)\n`,

  'Inverted':
    'ROTATE(3)\n' +
    'CLEAR(0)\n' +
    `FONT(${centre(5, 6)},24,'{H:02d}:{N:02d}',color=1,bg=0,scale=6)\n` +
    `FONT(${centre(10, 2)},86,'{y}-{m:02d}-{d:02d}',color=1,bg=0,scale=2)\n`,

  'Framed card':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    'RECT(3,3,246,118,width=2)\n' +
    'LINE(3,74,246,74)\n' +
    `FONT(${centre(5, 6)},22,'{H:02d}:{N:02d}',scale=6)\n` +
    `FONT(${centre(3, 2)},84,'{W}',scale=2)\n` +
    `FONT(${centre(10, 2)},100,'{y}-{m:02d}-{d:02d}',scale=2)\n`,

  'With seconds':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(8, 4)},30,'{H:02d}:{N:02d}:{S:02d}',scale=4)\n` +
    `FONT(${centre(10, 2)},80,'{y}-{m:02d}-{d:02d}',scale=2)\n`,

  /* Shows off the calendar variables. Widths are fixed rather than centred:
   * {M} and {W} are always three glyphs, and {j}/{V} are padded, so the line
   * does not reflow as the date changes - a face that shifts sideways on the
   * 1st of the month looks broken even though it is not. */
  'Calendar':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    "FONT(10,8,'{W} {d:02d} {M} {y}',scale=2)\n" +
    'LINE(10,30,239,30)\n' +          /* 239 = 249 - 10, so both margins match */
    `FONT(${centre(5, 6)},44,'{H:02d}:{N:02d}',scale=6)\n` +
    "FONT(10,102,'WEEK {V:02d} OF {G}')\n" +
    /* Right-aligned against the same 239 the rule above ends at. Always 11
     * glyphs: {j} is padded to 3, and {J} is 365 or 366 - so the alignment
     * holds all year rather than only after the 9th of January. */
    `FONT(${240 - textWidth(11, 1)},102,'DAY {j:03d}/{J}')\n`,

  /* Draws itself rather than just labelling itself: the fill is an expression
   * over {d} and {D}, so it grows across the month and resets on the 1st.
   *
   * The frame is fixed at x 4..245 - a 4 px margin at both ends of the 250 px
   * landscape frame, whose last column is 249. Scaling the frame by the month
   * length instead (4+{D}*8) both overran the right edge in a 31-day month and
   * made the bar itself change size between February and July, which is the
   * opposite of what a progress bar should do.
   *
   * {d}*241/{D} multiplies before dividing on purpose: the arithmetic is
   * integer, so dividing first would collapse the fraction to 0 or 1 and the
   * bar would jump rather than creep. */
  'Month progress':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    "FONT(4,4,'{H:02d}:{N:02d}',scale=3)\n" +
    "FONT(4,44,'{W} {d} {M} {y}',scale=2)\n" +
    'RECT(4,70,245,82)\n' +
    'RECT(4,70,4+{d}*241/{D},82,fill=1)\n' +
    "FONT(4,92,'DAY {j} OF {J}   WEEK {V}')\n",

  /* A real month grid, with today boxed out.
   *
   * The highlight is one INVERT rather than a filled RECT plus the number
   * redrawn in white: inverting whatever is already there does not need to
   * know which number it is covering, so it stays one line and keeps working
   * if the grid geometry moves. It has to come last - the 5x7 font paints its
   * whole glyph cell, so any day drawn afterwards would blank the box.
   *
   * Today's cell needs no column arithmetic at all: the column of day {d} is
   * {w}, by definition.
   *
   * EVERY(1440) is the point of the face. Nothing here changes until the date
   * does, and the default of a repaint a minute would spend 1439 full panel
   * refreshes a day redrawing identical pixels - by far the most expensive
   * thing this tag does. */
  'Month grid':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    'EVERY(1440)\n' +
    `FONT(${centre(8, 2)},2,'{M} {y}',scale=2)\n` +
    "FONT(8,20,'S  M  T  W  T  F  S')\n" +
    'LINE(4,29,245,29)\n' +
    monthGrid() +
    `INVERT(6+{w}*34,30+((${FIRST_COL}+{d}-1)/7)*14,20,13)\n`,

  /* Portrait, so the frame is 122x250 and the centring above does not apply. */
  'Portrait':
    'ROTATE(0)\n' +
    'CLEAR(1)\n' +
    "FONT(16,60,'{H:02d}',scale=3)\n" +
    "FONT(16,100,'{N:02d}',scale=3)\n" +
    "FONT(13,150,'{y}-{m:02d}-{d:02d}')\n" +
    "FONT(44,170,'{W}')\n",
};
