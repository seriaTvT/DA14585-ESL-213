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

/* color: 0 = black, 1 = white (matches the vendor DSL's own convention,
 * see function_doc_official.txt) */
void epd_gfx_clear(uint8_t color);
void epd_gfx_set_pixel(int16_t x, int16_t y, uint8_t color);
void epd_gfx_line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix);
void epd_gfx_rect(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t color, uint8_t pix, uint8_t filled);
void epd_gfx_circle(int16_t x, int16_t y, int16_t r, uint8_t color, uint8_t pix, uint8_t filled);

/* Minimal built-in 5x7 ASCII font fallback - NOT the vendor's own font
 * format (that uses custom PCtoLCD2002-built glyph tables per font_id,
 * see PROTOCOL_NOTES.md section 9, which we have not reverse engineered).
 * Good enough to prove the FONT() command path end-to-end; swap in real
 * glyph tables per font_id later if pixel-identical rendering matters. */
void epd_gfx_text(int16_t x, int16_t y, const char *text, uint8_t fore, uint8_t back, uint8_t scale);

/* Fill epd_framebuffer with a self-test image: a border, two diagonals, a
 * filled + an outline rectangle, a circle, and a line of digits. Use it for
 * first-flash bring-up — call this then epd_display(epd_framebuffer) to prove
 * the whole SPI/GPIO/driver path works before any BLE is involved. */
void epd_gfx_test_pattern(void);

#endif // _EPD_GFX_H_
