/**
 * epd_cmdparser.h - parser for the vendor's ASCII drawing command language.
 *
 * Implements a subset of the DSL documented in
 * the vendor's own login-gated documentation, transcribed and analysed in
 * PROTOCOL_NOTES.md section 4:
 *   CLEAR, RECT, LINE, CIRCLE, POINT, FONT
 * Not yet implemented (see PROTOCOL_NOTES.md for full command list):
 *   CAL, CLOCK, TABLE, IMG, ICON, ROTATE, MIRROR, SHOW, INV, LET, SRAND,
 *   RANDS, DATE_OFF, TIME_OFF, and the {} template-variable substitution
 *   engine. Those are meaningful follow-up work, not stubbed here by
 *   accident - they need either the real-time-clock plumbing, a numeric
 *   expression evaluator, or the flash-resident font/icon asset format,
 *   none of which exist in this skeleton yet.
 *
 * Commands arrive newline-terminated over the BLE command characteristic,
 * exactly as documented ("每条函数以换行符结尾"), so this parser can be fed
 * directly with the bytes from a CUSTS1_VAL_WRITE_IND on the command
 * characteristic.
 */

#ifndef _EPD_CMDPARSER_H_
#define _EPD_CMDPARSER_H_

#include <stdint.h>

/** Parse and execute one or more newline-separated commands from `buf`
 *  (length `len`, not necessarily NUL-terminated - BLE writes aren't).
 *  Drawing commands mutate epd_framebuffer directly; nothing is pushed to
 *  the physical panel here (call epd_display() separately once a batch of
 *  commands has been applied, same as the vendor firmware does). */
void epd_cmd_process(const uint8_t *buf, uint16_t len);

#endif // _EPD_CMDPARSER_H_
