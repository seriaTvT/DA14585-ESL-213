/**
 * epd_cmdparser.h - the tag's drawing language.
 *
 * A face is a short ASCII script, newline terminated, stored on the tag and
 * re-run on a timer so that {} variables re-expand and the picture keeps up
 * with the clock. That is what makes it a clock rather than an image.
 *
 *   CLEAR   fill the frame
 *   POINT   LINE   RECT   CIRCLE   INVERT      geometry
 *   TEXT    a string, in one of two fonts
 *   ROTATE  screen orientation, in degrees
 *   EVERY   how often to repaint
 *   TIME    RESET                              control, applied and not stored
 *
 * Numeric arguments are integer expressions - + - * / %, parentheses, unary
 * minus - and {} variables work inside them as well as inside text, so a face
 * can draw itself rather than only label itself. Required geometry is
 * positional; everything else is named and optional, which is what lets an
 * option be added later without disturbing a face already on a tag.
 *
 * Nothing throws. A malformed expression, an unknown variable and division by
 * zero all evaluate to 0, and an unrecognised line is skipped. A shelf label
 * with no host in range has to keep drawing something, so it degrades to a
 * wrong-looking face rather than a hung one - and the problems are counted
 * and reported, so that forgiveness does not also mean silence. See the
 * status report at the bottom of this file.
 *
 * The language began as a subset of the vendor's, which is where the shape of
 * it comes from and why PROTOCOL_NOTES.md is worth reading for background.
 * It is not that any more: the names that were actively misleading have been
 * changed, the dead arguments dropped, and the parts of their list that this
 * does not implement are out of scope rather than pending. Read this file for
 * what the language is; read theirs only for where it came from.
 */

#ifndef _EPD_CMDPARSER_H_
#define _EPD_CMDPARSER_H_

#include <stdint.h>
#include <stdbool.h>

/** Append received bytes to the stored script. `buf`/`len` need not be
 *  NUL-terminated or aligned to command boundaries - BLE writes aren't, since
 *  an ATT write carries only MTU-3 bytes (20 at the default 23-byte MTU)
 *  while several DSL commands are longer than that.
 *
 *  Nothing is executed here; call epd_cmd_run() once the batch is complete. */
void epd_cmd_feed(const uint8_t *buf, uint16_t len);

/** Execute the whole stored script against epd_framebuffer.
 *
 *  Safe and intended to be called repeatedly: the minute tick replays the
 *  same script so the {H}/{N}/... substitutions inside FONT() re-expand to
 *  the current time. Scripts normally open with CLEAR(), which makes each
 *  replay a full repaint from a known state.
 *
 *  Only touches the framebuffer - call epd_display() afterwards to push it. */
void epd_cmd_run(void);

/** Arm a fresh script: the *next* epd_cmd_feed() clears the buffer before
 *  appending, and later writes in that batch append as usual.
 *
 *  Deferring the clear this way is what keeps the tag displaying between
 *  sessions. A client is expected to push its whole template, so its bytes
 *  must not land underneath the previous one - but a client that connects and
 *  sends nothing (a scan, a failed sync, a dropped link) must leave the face
 *  it found intact. Clearing eagerly on connect would blank the panel until
 *  someone re-sent a template. Call this on connect. */
void epd_cmd_begin_batch(void);

/** Load the built-in clock face into the script buffer, replacing whatever is
 *  there. Call at boot so a tag with no host shows a clock (reading 00:00 until
 *  a TIME() sync) instead of a blank panel; a client's own template overwrites
 *  it on the next batch. */
void epd_cmd_load_default(void);

/* No epd_cmd_reset(). It dropped the stored script IMMEDIATELY, and nothing
 * ever called it: RESET() and the connect path both go through
 * epd_cmd_begin_batch(), which defers the clear to the first drawing write.
 * That deferral is the point - it is what lets a TIME()-only batch leave the
 * current face untouched - so an eager reset is not a simpler spelling of the
 * same thing, it is the bug that deferral was introduced to fix. Removed
 * 2026-08-13; use epd_cmd_begin_batch(). */

/* No epd_cmd_script_truncated(). Truncation is still tracked, and a client
 * still learns about it - epd_cmd_status() reports it as bit 0 of byte 5, which
 * webui/ble.js decodes as `truncated`. That is the route that has a consumer;
 * this accessor never did. Removed 2026-08-13. */

/** Bytes currently stored (0 = nothing to render). */
uint16_t epd_cmd_script_len(void);

/** The stored script itself, for persisting it. Not NUL-terminated; use
 *  epd_cmd_script_len(). Valid until the next feed. */
const char *epd_cmd_script(void);

/** Minutes between repaints the current script asked for via EVERY(), 1 if it
 *  asked for nothing. Set while the script runs, so it is only meaningful
 *  after the first epd_cmd_run() - which is fine, because the caller has to
 *  render once before it can skip anything.
 *
 *  Deciding whether a repaint is due is the caller's job: this reports the
 *  face's wish, it does not keep time. */
uint16_t epd_cmd_every_min(void);

/** Supply the panel temperature that {T} renders, in whole degrees Celsius.
 *
 *  Call before epd_cmd_run(), since the script is expanded there. Until it has
 *  been called at least once {T} is not a known name and renders literally, so
 *  a face asking for a temperature on a build with no sensor says so on the
 *  panel instead of showing a confident zero. */
void epd_cmd_set_temp(int8_t c);

/** Supply the battery reading that {BAT} and {VCC} render: charge in percent
 *  (clamped to 100) and terminal voltage in millivolts.
 *
 *  Same contract as epd_cmd_set_temp() - call before epd_cmd_run(), and until
 *  it has been called neither name is known, so a face asking for a battery
 *  reading on a build that does not take one renders "{BAT}" literally rather
 *  than a confident 0%.
 *
 *  Both come from one caller because they come from one measurement; a face
 *  wanting a bar wants the percentage, and one wanting to see a cell age wants
 *  the millivolts, which the percentage's curve has already flattened. */
void epd_cmd_set_batt(uint8_t pct, uint16_t mv);

/** Replace the stored script wholesale, e.g. with one restored from flash.
 *  Does not mark the script dirty - it is already persisted. */
void epd_cmd_load_script(const char *buf, uint16_t len);

/** True once if a client changed the script since the last call, i.e. there is
 *  something new worth writing to flash. Self-clearing, so a caller that acts
 *  on it will not write the same template twice. */
bool epd_cmd_take_dirty(void);

/** True if bytes have arrived since the last newline, i.e. a line is half in.
 *
 *  A caller that decides "the batch looks finished" from a gap in the writes must
 *  consult this, because a gap and a dangling line together mean the opposite: the
 *  gap fell inside a command. epd_cmd_run() has to commit the partial line before
 *  it can render - a client's final line may genuinely arrive without a newline -
 *  and the result is then persisted, so a half-written command becomes a permanent
 *  broken line in the stored face. Wait longer instead. */
bool epd_cmd_line_pending(void);

/* ---------------------------------------------------------------------------
 * Reporting what the last render made of the script
 *
 * The parser stays forgiving - a shelf label with no host in range has to keep
 * drawing something, so a bad line is skipped rather than treated as fatal and
 * a malformed argument evaluates to 0. That is right for the panel and useless
 * for whoever is writing the face: a typo and a deliberate choice look
 * identical from the outside.
 *
 * So the problems are counted as they are skipped, and handed to a client over
 * the status characteristic. This is the tag's own account of what it did,
 * which is worth more than the preview's prediction - the preview is a model
 * of the parser, and the whole reason it is tested against the firmware is
 * that models drift.
 * ------------------------------------------------------------------------- */

typedef enum {
    EPD_ERR_NONE = 0,
    EPD_ERR_UNKNOWN_CMD,    /* no command matched; the line drew nothing   */
    EPD_ERR_UNKNOWN_OPT,    /* `name=` the command does not read           */
    EPD_ERR_LINE_TOO_LONG,  /* over CMD_LINE_MAX; dropped whole            */
    EPD_ERR_SCRIPT_FULL,    /* the batch overran the script buffer         */
    EPD_ERR_BAD_ARG,        /* an argument the command cannot make sense of */
} epd_err_t;

/** Length of the status report written by epd_cmd_status(). */
#define EPD_STATUS_LEN  14

/** Fill `out` with the status of the most recent epd_cmd_run():
 *
 *   [0] format version of this report (currently 2)
 *   [1] epd_err_t of the FIRST problem found
 *   [2] line number of that problem, low byte  (1-based, 0 if not a line)
 *   [3] line number, high byte
 *
 * The line number counts lines of the *stored script*, which is not what the
 * client sent: TIME() and RESET() are applied on arrival and never stored, so
 * everything after them shifts up. A client that strips comments or blank
 * lines before sending (webui does) shifts them further. Translating back to
 * whatever the author is looking at is the client's job - it is the only side
 * that knows what it removed.
 *   [4] number of problems found, saturating at 255
 *   [5] flags: bit0 script truncated, bit1 a line was over CMD_LINE_MAX
 *   [6] stored script length, low byte
 *   [7] stored script length, high byte
 *   [8] repaint interval in minutes, low byte   (format 2 and later)
 *   [9] repaint interval, high byte
 *  [10] panel width in pixels, low byte        (format 3 and later)
 *  [11] panel width, high byte
 *  [12] panel height in pixels, low byte
 *  [13] panel height, high byte
 *
 * The geometry is here because it is no longer knowable from the firmware
 * revision string: one image drives both panels and learns which at boot, so
 * the build cannot state it. A client needs it - the image service takes a raw
 * 1bpp framebuffer, and the wrong size is not a rendering error but an
 * unusable transfer.
 *
 * Only the first problem is located, not all of them: an author fixes one and
 * pushes again, and carrying a list would cost buffer the script needs more.
 *
 * Byte [0] is why the interval could be appended without breaking anything: a
 * client reads it, takes the fields it knows and ignores the rest. Append only
 * - never renumber - or a tag and a client from different builds will disagree
 * silently, which is the one failure mode this byte exists to prevent. */
void epd_cmd_status(uint8_t out[EPD_STATUS_LEN]);

#endif // _EPD_CMDPARSER_H_
