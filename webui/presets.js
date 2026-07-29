/*
 * presets.js - starting-point faces.
 *
 * Kept out of app.js so they can be rendered headlessly by a test, which is
 * how the "byte-identical to the firmware's default" claim below stays true
 * rather than merely intended.
 */
/* Two panels exist and their faces are written separately rather than scaled,
 * because the small one is not a smaller version of the same layout - lines
 * that fit across 250 px do not fit across 212, and the answer is usually a
 * different line rather than a smaller one. See PANELS in epd.js.
 *
 *   high  landscape frame 250 x 122, middle x=125
 *   low   landscape frame 212 x 104, middle x=106
 *
 * Text metrics, needed to check anything fits: n glyphs at `scale` are
 * scale*(6n-1) px wide and 7*scale tall in the 5x7 font, scale*(17n-1) by
 * 24*scale in font=1. Faces centre with align=1 rather than with offsets
 * computed by hand, which is what this file used to do and what stopped being
 * correct the moment a second font existed. */
const MID = 125;
const MID_LOW = 106;

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

function monthGrid({ x0, colW, y0, rowH }) {
  let s = '';
  for (let n = 1; n <= 31; n++) {
    /* Days 29-31 do not exist in every month, and there is no way to skip a
     * line. n/({D}+1) is 0 while the day is real and 1 once it is past the
     * end of the month, so the number is simply pushed off the panel and
     * clipped - February stops at 28 without a conditional. */
    const off = n >= 29 ? `+(${n}/({D}+1))*200` : '';
    s += `TEXT(${x0}+${dayCol(n)}*${colW},${y0}+${dayRow(n)}*${rowH}${off},'${n}')\n`;
  }
  return s;
}

/* Weekday header for the low-res grid. The high-res face spaces its letters
 * with runs of spaces inside one string, which works only because its 34 px
 * columns happen to be near a whole number of 6 px glyph pitches. At 29 px they
 * are not, so the letters are placed individually - seven short lines rather
 * than one string that drifts a pixel further out of true every column. */
function dowHeader({ x0, colW, y }) {
  const glyphMid = Math.round((colW - 5) / 2);
  return ['S', 'M', 'T', 'W', 'T', 'F', 'S']
    .map((c, i) => `TEXT(${x0 + glyphMid + i * colW},${y},'${c}')\n`)
    .join('');
}

const HIGH = {
  /* Byte-identical to DEFAULT_FACE[] in epd_cmdparser.c, so "what the tag
   * ships with" is always one click away - and, since a test diffs the two,
   * stays that way if either side is edited. */
  'Built-in default':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID},18,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    `TEXT(${MID},78,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n` +
    `TEXT(${MID},100,'{W}',scale=2,align=1)\n`,

  /* The face this shipped with before the 16x24 font existed, kept because
   * the blocky 5x7 digits are a look and not only a limitation.
   *
   * Coordinates are the original hand-computed ones rather than align=1,
   * which would land a pixel to the right: the old faces centred with
   * floor((250 - 145) / 2) = 52, while align=1 at x=125 computes
   * 125 - 145/2 = 53. Invisible in use, but this preset exists to be the old
   * face, so it is the old face. */
  'Classic':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(52,25,'{H:02d}:{N:02d}',scale=5)\n" +
    "TEXT(66,72,'{y}-{m:02d}-{d:02d}',scale=2)\n" +
    "TEXT(108,94,'{W}',scale=2)\n",

  /* Nothing but the time, as large as the panel takes. font=1 at scale 2 is
   * 168x48 for HH:MM; scale 3 would be 252 wide against a 250 px frame, so
   * this is the ceiling rather than a preference. Centred on both axes:
   * (122 - 48) / 2 = 37. */
  'Big clock':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID},34,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n`,

  /* The old big clock: 5x7 at scale 7 is 203 px wide against font=1 scale 2's
   * 168, so this is still the widest HH:MM available - just a coarser one.
   * Original offsets again, for the same reason as Classic above. */
  'Classic big clock':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(23,22,'{H:02d}:{N:02d}',scale=7)\n" +
    "TEXT(42,88,'{W} {y}-{m:02d}-{d:02d}',scale=2)\n",

  'Inverted':
    'ROTATE(270)\n' +
    'CLEAR(0)\n' +
    `TEXT(${MID},20,'{H:02d}:{N:02d}',font=1,scale=2,color=1,bg=0,align=1)\n` +
    `TEXT(${MID},86,'{y}-{m:02d}-{d:02d}',color=1,bg=0,scale=2,align=1)\n`,

  'Framed card':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'RECT(3,3,246,118,width=2)\n' +
    'LINE(3,74,246,74)\n' +
    `TEXT(${MID},14,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    `TEXT(${MID},81,'{W}',scale=2,align=1)\n` +
    `TEXT(${MID},99,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n`,

  /* Seconds rule out the large font - it has no 'S' problem, but eight glyphs
   * at font=1 would be 271 px wide. The 5x7 font at scale 4 fits. */
  'With seconds':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID},30,'{H:02d}:{N:02d}:{S:02d}',scale=4,align=1)\n` +
    `TEXT(${MID},80,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n`,

  /* Shows off the calendar variables. The two bottom labels are anchored to
   * the margins rather than centred - align=0 on the left, align=2 on the
   * right - so they stay put as the numbers change width. */
  'Calendar':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(8,8,'{W} {d:02d} {M} {y}',scale=2)\n" +
    'LINE(8,30,239,30)\n' +         /* 239 = 249 - 10, so both margins match */
    `TEXT(${MID},38,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    "TEXT(10,102,'WEEK {V:02d} OF {G}')\n" +
    "TEXT(235,102,'DAY {j:03d}/{J}',align=2)\n",

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
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(12,12,'{H:02d}:{N:02d}',scale=3)\n" +
    "TEXT(12,46,'{W} {d} {M} {y}',scale=2)\n" +
    'RECT(4,70,245,82)\n' +
    'RECT(4,70,4+{d}*241/{D},82,fill=1)\n' +
    "TEXT(12,94,'DAY {j} OF {J}   WEEK {V}')\n",

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
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'EVERY(1440)\n' +
    `TEXT(${MID},10,'{M} {y}',scale=2,align=1)\n` +
    "TEXT(8,30,'S     M    T     W     T     F    S')\n" +
    'LINE(4,39,245,39)\n' +
    monthGrid({ x0: 8, colW: 34, y0: 42, rowH: 14 }) +
    `INVERT(4+{w}*34,39+((${FIRST_COL}+{d}-1)/7)*14,20,13)\n`,

  /* Portrait, so the frame is 122x250 and MID does not apply - 61 is the
   * middle here. Hours and minutes stack because HH:MM will not fit across
   * 122 px at a size worth having. */
  'Portrait':
    'ROTATE(0)\n' +
    'CLEAR(1)\n' +
    "TEXT(61,56,'{H:02d}',font=1,scale=2,align=1)\n" +
    "TEXT(61,108,'{N:02d}',font=1,scale=2,align=1)\n" +
    "TEXT(61,170,'{y}-{m:02d}-{d:02d}',align=1)\n" +
    "TEXT(61,186,'{W}',align=1)\n",

  /* Thermometer. The tag can report its own panel temperature ({T}, whole
   * degrees Celsius), so this face makes that the headline and keeps the clock
   * underneath it.
   *
   * The glyph is drawn rather than typed: the fonts are uppercase ASCII and
   * have no degree sign, let alone a thermometer. A filled bulb, an outlined
   * stem with a filled column inside it, and three ticks read as one at a
   * glance and cost five commands.
   *
   * {T} renders literally as "{T}" on a build with no temperature reading -
   * see EPD_TEMP_READ - which is deliberate: it says "this tag cannot measure
   * that" rather than showing a confident 0. */
  'Thermometer':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'CIRCLE(32,98,12,color=0,fill=1)\n' +
    'RECT(26,26,38,92,color=0,width=2,fill=0)\n' +
    'RECT(29,55,35,92,color=0,fill=1)\n' +
    'LINE(44,40,56,40,0,2)\n' +
    'LINE(44,58,56,58,0,2)\n' +
    'LINE(44,76,56,76,0,2)\n' +
    "TEXT(165,22,'{T}C',scale=4,align=1)\n" +
    "TEXT(165,62,'{H:02d}:{N:02d}',scale=3,align=1)\n" +
    "TEXT(165,95,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n",
};

/* ---- low-res faces (212 x 104 landscape) -----------------------------------
 * Not the high-res faces rescaled. The frame is 38 px narrower and 18 px
 * shorter, which is enough to break the lines rather than merely crowd them:
 * 'WEDNESDAY 2026-07-29' at scale 2 is 238 px and simply does not go, so the
 * faces that carried it lose a field or drop a scale instead.
 *
 * The big font is unchanged across panels, so HH:MM at font=1 scale=2 is still
 * 168x48 and still the centrepiece - it just leaves 56 px of height for
 * everything else here rather than 74. */
const LOW = {
  /* Byte-identical to DEFAULT_FACE[] built with EPD_PANEL_LOW_RES, the same
   * way the high-res one is - and diffed against it by the same test. */
  'Built-in default':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID_LOW},8,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    `TEXT(${MID_LOW},64,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n` +
    `TEXT(${MID_LOW},84,'{W}',scale=2,align=1)\n`,

  /* The 5x7 face. scale=5 gives a 145x35 HH:MM, which still has 33 px of slack
   * across this frame - the coarse digits were never the width problem. */
  'Classic':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID_LOW},14,'{H:02d}:{N:02d}',scale=5,align=1)\n` +
    `TEXT(${MID_LOW},60,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n` +
    `TEXT(${MID_LOW},80,'{W}',scale=2,align=1)\n`,

  /* Nothing but the time. Centred on both axes: (104 - 48) / 2 = 28. */
  'Big clock':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID_LOW},28,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n`,

  /* 5x7 at scale 7 is 203 px against a 212 px frame - a 4 px margin each side,
   * and still the widest HH:MM available. What does not survive is the date
   * line under it: '{W} {y}-{m:02d}-{d:02d}' would be 238 px, so it becomes
   * day and month instead of the full date. */
  'Classic big clock':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID_LOW},16,'{H:02d}:{N:02d}',scale=7,align=1)\n` +
    `TEXT(${MID_LOW},76,'{W} {d} {M}',scale=2,align=1)\n`,

  'Inverted':
    'ROTATE(270)\n' +
    'CLEAR(0)\n' +
    `TEXT(${MID_LOW},16,'{H:02d}:{N:02d}',font=1,scale=2,color=1,bg=0,align=1)\n` +
    `TEXT(${MID_LOW},76,'{y}-{m:02d}-{d:02d}',color=1,bg=0,scale=2,align=1)\n`,

  /* Frame inset 3 px like the high-res card, against a last column of 211 and
   * a last row of 103. */
  'Framed card':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'RECT(3,3,208,100,width=2)\n' +
    'LINE(3,60,208,60)\n' +
    `TEXT(${MID_LOW},6,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    `TEXT(${MID_LOW},66,'{W}',scale=2,align=1)\n` +
    `TEXT(${MID_LOW},82,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n`,

  /* HH:MM:SS at 5x7 scale 4 is 188 px, so seconds still fit across this frame.
   * The pair is centred as a block: (104 - 28 - 12 - 14) / 2 = 25. */
  'With seconds':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    `TEXT(${MID_LOW},25,'{H:02d}:{N:02d}:{S:02d}',scale=4,align=1)\n` +
    `TEXT(${MID_LOW},65,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n`,

  /* The high-res calendar puts weekday, day, month and year on one scale-2 line
   * (250 px worth). That is 21 glyphs and will not go here, so the header
   * splits: weekday on the left margin, day and month on the right, both
   * anchored so they stay put as the numbers change width. */
  'Calendar':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(8,6,'{W}',scale=2)\n" +
    "TEXT(203,6,'{d:02d} {M}',scale=2,align=2)\n" +
    'LINE(8,26,203,26)\n' +
    `TEXT(${MID_LOW},32,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    "TEXT(8,88,'WEEK {V:02d} OF {G}')\n" +
    "TEXT(203,88,'DAY {j:03d}/{J}',align=2)\n",

  /* Same idea as the high-res bar, re-fitted: the frame is x 4..207 - a 4 px
   * margin against a last column of 211 - so the fill spans 203 px and the
   * multiply still comes before the divide, or integer arithmetic would
   * collapse the fraction and the bar would jump rather than creep. */
  'Month progress':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    "TEXT(10,10,'{H:02d}:{N:02d}',scale=3)\n" +
    "TEXT(10,38,'{W} {d} {M}',scale=2)\n" +
    'RECT(4,60,207,72)\n' +
    'RECT(4,60,4+{d}*203/{D},72,fill=1)\n' +
    "TEXT(10,80,'DAY {j} OF {J}   WEEK {V}')\n",

  /* 7 columns of 29 px span 203 of the 212, leaving a 4 px margin each side.
   * Rows are 11 px rather than 14, which is what keeps six of them inside a
   * 104 px frame: 38 + 5*11 + 7 = 100. */
  'Month grid':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'EVERY(1440)\n' +
    `TEXT(${MID_LOW},4,'{M} {y}',scale=2,align=1)\n` +
    dowHeader({ x0: 4, colW: 29, y: 25 }) +
    'LINE(4,34,207,34)\n' +
    monthGrid({ x0: 8, colW: 29, y0: 38, rowH: 11 }) +
    `INVERT(4+{w}*29,37+((${FIRST_COL}+{d}-1)/7)*11,18,10)\n`,

  /* Portrait: the frame is 104x212 and the middle is x=52. The date drops to
   * scale 1 - at scale 2 it would be 118 px against a 104 px frame. */
  'Portrait':
    'ROTATE(0)\n' +
    'CLEAR(1)\n' +
    "TEXT(52,36,'{H:02d}',font=1,scale=2,align=1)\n" +
    "TEXT(52,92,'{N:02d}',font=1,scale=2,align=1)\n" +
    "TEXT(52,156,'{y}-{m:02d}-{d:02d}',align=1)\n" +
    "TEXT(52,169,'{W}',align=1)\n",

  /* Thermometer, same idea in 212x104. The glyph shrinks and the date drops to
   * make room: 10 characters at scale 2 is 120 px, which still goes beside a
   * narrower thermometer, but only just. */
  'Thermometer':
    'ROTATE(270)\n' +
    'CLEAR(1)\n' +
    'CIRCLE(26,84,10,color=0,fill=1)\n' +
    'RECT(21,22,31,78,color=0,width=2,fill=0)\n' +
    'RECT(23,48,29,78,color=0,fill=1)\n' +
    'LINE(36,34,46,34,0,2)\n' +
    'LINE(36,50,46,50,0,2)\n' +
    "TEXT(135,18,'{T}C',scale=3,align=1)\n" +
    "TEXT(135,48,'{H:02d}:{N:02d}',scale=3,align=1)\n" +
    "TEXT(135,80,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n",
};

/** Faces by panel key, matching PANELS in epd.js. */
export const PRESETS = { high: HIGH, low: LOW };
