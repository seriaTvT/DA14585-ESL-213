/**
 * epd_gfx.h - 1bpp framebuffer + drawing primitives.
 *
 * Pixel packing matches the vendor's own client-side encoder
 * (canvas2bytes() in dithering.js, see PROTOCOL_NOTES.md section 6):
 * row-major, MSB-first, 1 = white, 0 = black. This means images pushed by
 * the existing esl_clock.php web tool (or img2data.php) land in the
 * framebuffer with no repacking needed.
 */

#ifndef _EPD_GFX_H_
#define _EPD_GFX_H_

#include <stdint.h>
#include "epd_ssd1680.h"

extern uint8_t epd_framebuffer[EPD_BUF_SIZE];

/* ---- screen rotation -------------------------------------------------------
 * Quarter-turns clockwise: 0 = native portrait, 1 = 90 deg, 2 = 180, 3 = 270.
 * Odd values give landscape - 250x122 instead of 122x250 - which is how these
 * tags normally sit on a shelf; the vendor's own ROTATE() defaults to 270.
 *
 * The transform is applied in the framebuffer write, so it costs nothing at
 * push time: epd_display() still sends the same native-orientation buffer and
 * the panel driver is untouched. (The SSD1680 cannot do this itself - its Data
 * Entry Mode 0x11 only mirrors/flips axes, it cannot transpose them, because a
 * RAM byte is always 8 pixels along the panel's own X axis.)
 *
 * All drawing coordinates are in the ROTATED frame, so bounds come from
 * epd_gfx_width()/epd_gfx_height(), never EPD_WIDTH/EPD_HEIGHT. */
void epd_gfx_set_rotation(uint8_t r);
uint8_t epd_gfx_get_rotation(void);
int16_t epd_gfx_width(void);
int16_t epd_gfx_height(void);

/* ---- what changed, for a partial refresh ------------------------------------
 * Compare two framebuffers and report the band of rows that differ: `*first`
 * and `*last` inclusive. Returns false if they are identical, in which case
 * neither output is written and there is nothing to send to the panel at all.
 *
 * A band of rows rather than a rectangle, and that is not laziness. Refresh time
 * on these panels is set by how many GATE lines are driven, not source lines:
 * the whole framebuffer is only ~4 ms of SPI at 8 MHz, so narrowing the X window
 * saves nothing measurable while narrowing Y is the entire win.
 *
 * A diff rather than tracking what the primitives touched, because the DSL
 * re-renders the whole frame from scratch on every repaint - CLEAR then draw -
 * so there is no incremental history to track. Diffing against what is on the
 * glass also cannot drift out of step with reality the way a dirty flag can.
 *
 * **Rows here are PHYSICAL rows, i.e. panel gate lines** - the one place in this
 * header that means EPD_HEIGHT rather than epd_gfx_height(). Rotation is applied
 * when pixels are written, so by the time a byte is in the framebuffer its row
 * index is already the panel's own Y and a band needs no transforming. Whatever
 * ROTATE() a face asked for, this returns a window the driver can use directly.
 */
bool epd_gfx_dirty_rows(const uint8_t *a, const uint8_t *b,
                        uint16_t *first, uint16_t *last);

/* color: 0 = black, 1 = white (matches the vendor DSL's own convention,
 * see function_doc_official.txt) */
void epd_gfx_clear(uint8_t color);
void epd_gfx_set_pixel(int16_t x, int16_t y, uint8_t color);
void epd_gfx_line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix);
void epd_gfx_rect(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix, uint8_t filled);
void epd_gfx_circle(int16_t x, int16_t y, int16_t r, uint8_t color, uint8_t pix, uint8_t filled);

/* Flip every pixel in a rectangle, corners inclusive and in either order.
 *
 * This is the one primitive that reads the framebuffer as well as writing it,
 * and that is the point: highlighting a calendar's "today" by drawing a filled
 * box and then re-drawing the number in the opposite colour needs the face to
 * know which number it is covering. Inverting whatever is already there needs
 * no such knowledge, so it costs one line instead of two and works over text,
 * rules and blank space alike. Order matters - invert last, or a later opaque
 * glyph cell will paint over it. */
void epd_gfx_invert(int16_t x1, int16_t y1, int16_t x2, int16_t y2);

/* Two fonts, both ours - drawn here rather than taken from the vendor, whose
 * format is per-font_id PCtoLCD2002 tables we never reverse engineered.
 *
 *   5x7    the general one: digits, uppercase, and the punctuation a clock or
 *          calendar face needs. Scales up in whole pixels.
 *   16x24  digits and ':' only, drawn at that size. For the case that wants
 *          it - an HH:MM face at scale 5 on a 5x7 glyph is a block of 5px
 *          squares and looks like one.
 *
 * A character the 16x24 table lacks draws blank rather than falling back to
 * 5x7: two glyph sizes in one string reads as a fault, while a gap reads as
 * one, and the preview names the character. */
#define EPD_FONT_5X7    0
#define EPD_FONT_16X24  1

void epd_gfx_text(int16_t x, int16_t y, const char *text, uint8_t fore,
                  uint8_t back, uint8_t scale, uint8_t font);

/* Pixel width epd_gfx_text() will occupy: ((w + 1)n - 1) * scale, where w is
 * the font's cell width - each glyph is followed by a 1 px gap and the last
 * gap is not drawn. 0 for empty text.
 *
 * Exists so align= can place text without the face hand-computing offsets.
 * The current faces carry arithmetic like (250 - width) / 2 worked out by hand
 * against the glyph metrics - which is silently wrong the moment those metrics
 * change, and changing them is exactly what adding a second font does. */
int16_t epd_gfx_text_width(const char *text, uint8_t scale, uint8_t font);

#endif // _EPD_GFX_H_
