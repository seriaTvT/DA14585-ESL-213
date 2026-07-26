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

/** Drop the stored script immediately, blanking the clock face at the next
 *  render. Not part of the connect/disconnect path - see epd_cmd_begin_batch(). */
void epd_cmd_reset(void);

/** True if the last batch exceeded the script buffer and was truncated. */
bool epd_cmd_script_truncated(void);

/** Bytes currently stored (0 = nothing to render). */
uint16_t epd_cmd_script_len(void);

#endif // _EPD_CMDPARSER_H_
