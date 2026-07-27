/*
 * presets.js - starting-point faces.
 *
 * Kept out of app.js so they can be rendered headlessly by a test, which is
 * how the "byte-identical to the firmware's default" claim below stays true
 * rather than merely intended.
 */
/* Landscape geometry throughout, so the frame is 250x122 and its middle is
 * x=125. Faces centre with align=1 against that rather than with offsets
 * computed from the glyph metrics, which is what this file used to do and
 * what stopped being correct the moment a second font existed. */
const MID = 125;

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
    s += `TEXT(8+${dayCol(n)}*34,32+${dayRow(n)}*14${off},'${n}')\n`;
  }
  return s;
}

export const PRESETS = {
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
    `TEXT(${MID},37,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n`,

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
    `TEXT(${MID},20,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    `TEXT(${MID},84,'{W}',scale=2,align=1)\n` +
    `TEXT(${MID},100,'{y}-{m:02d}-{d:02d}',scale=2,align=1)\n`,

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
    "TEXT(10,8,'{W} {d:02d} {M} {y}',scale=2)\n" +
    'LINE(10,30,239,30)\n' +         /* 239 = 249 - 10, so both margins match */
    `TEXT(${MID},44,'{H:02d}:{N:02d}',font=1,scale=2,align=1)\n` +
    "TEXT(10,102,'WEEK {V:02d} OF {G}')\n" +
    "TEXT(239,102,'DAY {j:03d}/{J}',align=2)\n",

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
    "TEXT(4,4,'{H:02d}:{N:02d}',scale=3)\n" +
    "TEXT(4,44,'{W} {d} {M} {y}',scale=2)\n" +
    'RECT(4,70,245,82)\n' +
    'RECT(4,70,4+{d}*241/{D},82,fill=1)\n' +
    "TEXT(4,92,'DAY {j} OF {J}   WEEK {V}')\n",

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
    `TEXT(${MID},2,'{M} {y}',scale=2,align=1)\n` +
    "TEXT(8,20,'S  M  T  W  T  F  S')\n" +
    'LINE(4,29,245,29)\n' +
    monthGrid() +
    `INVERT(6+{w}*34,30+((${FIRST_COL}+{d}-1)/7)*14,20,13)\n`,

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
};
