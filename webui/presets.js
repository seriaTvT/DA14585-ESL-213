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

export const PRESETS = {
  /* Byte-identical to DEFAULT_FACE[] in epd_cmdparser.c, so "what the tag
   * ships with" is always one click away - and, since a test diffs the two,
   * stays that way if either side is edited. */
  'Built-in default':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(5, 5)},25,0,0,0,1,5,'{H:02d}:{N:02d}')\n` +
    `FONT(${centre(10, 2)},72,0,0,0,1,2,'{y}-{m:02d}-{d:02d}')\n` +
    `FONT(${centre(3, 2)},94,0,0,0,1,2,'{W}')\n`,

  'Big clock':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(5, 7)},22,0,0,0,1,7,'{H:02d}:{N:02d}')\n` +
    `FONT(${centre(14, 2)},88,0,0,0,1,2,'{W} {y}-{m:02d}-{d:02d}')\n`,

  'Inverted':
    'ROTATE(3)\n' +
    'CLEAR(0)\n' +
    `FONT(${centre(5, 6)},24,0,0,1,0,6,'{H:02d}:{N:02d}')\n` +
    `FONT(${centre(10, 2)},86,0,0,1,0,2,'{y}-{m:02d}-{d:02d}')\n`,

  'Framed card':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    'RECT(3,3,246,118,0,2,0)\n' +
    'LINE(3,74,246,74,0,1)\n' +
    `FONT(${centre(5, 6)},22,0,0,0,1,6,'{H:02d}:{N:02d}')\n` +
    `FONT(${centre(3, 2)},84,0,0,0,1,2,'{W}')\n` +
    `FONT(${centre(10, 2)},100,0,0,0,1,2,'{y}-{m:02d}-{d:02d}')\n`,

  'With seconds':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    `FONT(${centre(8, 4)},30,0,0,0,1,4,'{H:02d}:{N:02d}:{S:02d}')\n` +
    `FONT(${centre(10, 2)},80,0,0,0,1,2,'{y}-{m:02d}-{d:02d}')\n`,

  /* Shows off the calendar variables. Widths are fixed rather than centred:
   * {M} and {W} are always three glyphs, and {j}/{V} are padded, so the line
   * does not reflow as the date changes - a face that shifts sideways on the
   * 1st of the month looks broken even though it is not. */
  'Calendar':
    'ROTATE(3)\n' +
    'CLEAR(1)\n' +
    "FONT(10,8,0,0,0,1,2,'{W} {d:02d} {M} {y}')\n" +
    'LINE(10,30,239,30,0,1)\n' +          /* 239 = 249 - 10, so both margins match */
    `FONT(${centre(5, 6)},44,0,0,0,1,6,'{H:02d}:{N:02d}')\n` +
    "FONT(10,102,0,0,0,1,1,'WEEK {V:02d} OF {G}')\n" +
    /* Right-aligned against the same 239 the rule above ends at. Always 11
     * glyphs: {j} is padded to 3, and {J} is 365 or 366 - so the alignment
     * holds all year rather than only after the 9th of January. */
    `FONT(${240 - textWidth(11, 1)},102,0,0,0,1,1,'DAY {j:03d}/{J}')\n`,

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
    "FONT(4,4,0,0,0,1,3,'{H:02d}:{N:02d}')\n" +
    "FONT(4,44,0,0,0,1,2,'{W} {d} {M} {y}')\n" +
    'RECT(4,70,245,82,0,1,0)\n' +
    'RECT(4,70,4+{d}*241/{D},82,0,1,1)\n' +
    "FONT(4,92,0,0,0,1,1,'DAY {j} OF {J}   WEEK {V}')\n",

  /* Portrait, so the frame is 122x250 and the centring above does not apply. */
  'Portrait':
    'ROTATE(0)\n' +
    'CLEAR(1)\n' +
    "FONT(16,60,0,0,0,1,3,'{H:02d}')\n" +
    "FONT(16,100,0,0,0,1,3,'{N:02d}')\n" +
    "FONT(13,150,0,0,0,1,1,'{y}-{m:02d}-{d:02d}')\n" +
    "FONT(44,170,0,0,0,1,1,'{W}')\n",
};
