/**
 * epd_cmdparser.h - parser for the vendor's ASCII drawing command language.
 *
 * Implements a subset of the DSL documented in
 * the vendor's own login-gated documentation, transcribed and analysed in
 * PROTOCOL_NOTES.md section 4:
 *   CLEAR, RECT, LINE, CIRCLE, POINT, FONT, ROTATE
 * plus the {} template-variable substitution engine (date/time subset) and
 * TIME(), our own extension - see epd_cmdparser.c.
 * Not yet implemented (see PROTOCOL_NOTES.md for full command list):
 *   CAL, CLOCK, TABLE, IMG, ICON, MIRROR, SHOW, INV, LET, SRAND,
 *   RANDS, DATE_OFF, TIME_OFF. Those are meaningful follow-up work, not
 *   stubbed here by accident - they need either a numeric expression
 *   evaluator or the flash-resident font/icon asset format, neither of
 *   which exists yet.
 *
 * Commands arrive newline-terminated over the BLE command characteristic,
 * exactly as documented ("每条函数以换行符结尾"), so this parser can be fed
 * directly with the bytes from a CUSTS1_VAL_WRITE_IND on the command
 * characteristic.
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

/** Drop the stored script immediately, blanking the clock face at the next
 *  render. Not part of the connect/disconnect path - see epd_cmd_begin_batch(). */
void epd_cmd_reset(void);

/** True if the last batch exceeded the script buffer and was truncated. */
bool epd_cmd_script_truncated(void);

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

/** Replace the stored script wholesale, e.g. with one restored from flash.
 *  Does not mark the script dirty - it is already persisted. */
void epd_cmd_load_script(const char *buf, uint16_t len);

/** True once if a client changed the script since the last call, i.e. there is
 *  something new worth writing to flash. Self-clearing, so a caller that acts
 *  on it will not write the same template twice. */
bool epd_cmd_take_dirty(void);

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
} epd_err_t;

/** Length of the status report written by epd_cmd_status(). */
#define EPD_STATUS_LEN  10

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
