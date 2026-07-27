/**
 * epd_ssd1680.h
 *
 * SSD1680-family e-paper controller driver for the Hema 2.13" ESL tag.
 *
 * Command sequence and LUT usage were reverse-engineered from the community
 * "5_hema_clock_down_*_V1.57.bin" firmware (see PROTOCOL_NOTES.md at the repo
 * root) and cross-checked against the panel vendor's own reference driver,
 * which is itself explicitly named after (i.e. ported from) Waveshare's public
 * EPD_2IN13_V2 driver for the same SSD1680-family controller.
 *
 * Panel identified (see PROTOCOL_NOTES.md section 2):
 *   low-res:  Good Display HINK-E0213A41-FPC / HINK-E0213A07-A1, 104 x 212 px
 *   high-res: Good Display HINK-E0213A53-FPC-A0,                122 x 250 px
 *
 * STATUS: verified on real hardware. The pin map below was recovered from the
 * community firmware, confirmed by continuity-testing the EPD FPC connector
 * back to the DA14585 package, and then proven end-to-end by rendering a test
 * pattern on the panel. Two places where this deliberately DIVERGES from
 * Waveshare's reference driver are marked in epd_ssd1680.c and were each
 * confirmed by photographing the result:
 *   - EPD_INVERT_OUTPUT is 0 (Waveshare inverts; its GUI layer uses the
 *     opposite 1=black convention to ours)
 *   - Data Entry Mode is 0x03 / Y-increment (Waveshare uses 0x01 /
 *     Y-decrement, which mirrors our top-down framebuffer vertically)
 */

#ifndef _EPD_SSD1680_H_
#define _EPD_SSD1680_H_

#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"

/* ------------------------------------------------------------------------
 * Panel geometry. This board's panel is HINK-E0213A53-FPC-A0 = the high-res
 * 122x250 variant (confirmed from the FPC label in the board photo), so
 * high-res is the default. To build for the low-res 104x212 panel instead,
 * define EPD_PANEL_LOW_RES project-wide.
 * ---------------------------------------------------------------------- */
#if defined(EPD_PANEL_LOW_RES)
    #define EPD_WIDTH   104
    #define EPD_HEIGHT  212
#else
    #define EPD_WIDTH   122
    #define EPD_HEIGHT  250
#endif

/* Bytes per row in the 1bpp framebuffer (width rounded up to a byte). */
#define EPD_WIDTH_BYTES  ((EPD_WIDTH + 7) / 8)
#define EPD_BUF_SIZE     (EPD_WIDTH_BYTES * EPD_HEIGHT)

/* ------------------------------------------------------------------------
 * BOARD VARIANT — set exactly one.
 *
 * Two ESL boards exist carrying the same DA14585 and the same SSD1680-family
 * panel, wired differently. The community firmware chose between them at
 * runtime from a stored config byte; we choose at build time.
 *
 *   VARIANT B  the first tag studied here. Pin map recovered from the
 *              community firmware and confirmed by continuity-testing the FPC
 *              connector back to the package.
 *   VARIANT A  the tag that arrived still running the original retail
 *              firmware. Pin map read straight out of that firmware's own
 *              live pin table at 0x07FD4428 in a SysRAM dump — see
 *              hema-local/re/newtag/README.md. Only CS is shared with B.
 *
 * GETTING THIS WRONG IS INVISIBLE AT BOOT. The tag starts, advertises and
 * accepts commands exactly as normal; the panel simply never changes. That is
 * precisely what happened when a variant-B build was flashed to the variant-A
 * tag, so if a board goes quiet on the panel alone, check this first.
 * ---------------------------------------------------------------------- */
#if !defined(EPD_BOARD_VARIANT_A) && !defined(EPD_BOARD_VARIANT_B)
    /* Default: the variant-A tag, which is the one on the bench. */
    #define EPD_BOARD_VARIANT_A
#endif

#if defined(EPD_BOARD_VARIANT_A)

/* ------------------------------------------------------------------------
 * VARIANT A pin map, read from the retail firmware's own table.
 *
 *   EPD signal   DA14585 GPIO   table entry
 *   ----------   ------------   -----------
 *   SCK            P0_1           +0x06
 *   SDA (MOSI)     P2_0           +0x08
 *   D/C            P0_7           +0x0A
 *   CS             P2_1           +0x00
 *   RST            P1_0           +0x04   (the only entry ever pulsed low)
 *   BUSY (input)   P1_1           +0x0C
 *   enable         P2_3           +0x0E   (held high)
 *   enable         P2_2           +0x02   (held high — note B drives it LOW)
 *
 * The panel is bit-banged here rather than driven by the hardware SPI block,
 * following the retail firmware, which bit-bangs these same two pins. It costs
 * nothing on this board and buys a real simplification: the panel pins are
 * disjoint from the boot flash's (P0_0/P0_3/P0_5/P0_6), so unlike variant B
 * there is no bus to share and no D/C-versus-MISO collision on P0_5.
 * ---------------------------------------------------------------------- */
#define EPD_BITBANG      1

#define EPD_SCK_PORT     GPIO_PORT_0
#define EPD_SCK_PIN      GPIO_PIN_1

#define EPD_SDA_PORT     GPIO_PORT_2
#define EPD_SDA_PIN      GPIO_PIN_0

#define EPD_DC_PORT      GPIO_PORT_0
#define EPD_DC_PIN       GPIO_PIN_7

#define EPD_RST_PORT     GPIO_PORT_1
#define EPD_RST_PIN      GPIO_PIN_0

#define EPD_BUSY_PORT    GPIO_PORT_1
#define EPD_BUSY_PIN     GPIO_PIN_1

#define EPD_CS_PORT      GPIO_PORT_2
#define EPD_CS_PIN       GPIO_PIN_1

#define EPD_PWR_PORT     GPIO_PORT_2
#define EPD_PWR_PIN      GPIO_PIN_3

/* Second enable line. Variant B's firmware drives its P2_2 low and we leave it
 * alone there; variant A's retail firmware holds it high, so we do too. */
#define EPD_AUX_PORT     GPIO_PORT_2
#define EPD_AUX_PIN      GPIO_PIN_2

#else   /* EPD_BOARD_VARIANT_B */

#define EPD_BITBANG      0

/* ------------------------------------------------------------------------
 * VARIANT B pin assignments — RECOVERED FROM THE COMMUNITY FIRMWARE.
 *
 * These are no longer guesses. They were extracted from the community
 * `5_hema_clock_down_high_V1.57.bin` by decompiling both the pin-setup
 * function and the actual command/data/reset routines with a correctly
 * memory-mapped Ghidra project, and the two agree (see PROTOCOL_NOTES.md
 * §13). The firmware supports two board variants selected by a stored
 * config byte; the values below are "variant B" (config byte == 0), which
 * is the fully cross-checked one and whose control pins sit on physically
 * adjacent package pins (7/8/9/10) — consistent with a real board layout.
 *
 *   EPD signal   DA14585 GPIO   QFN40 pin
 *   ----------   ------------   ---------
 *   SCK  (CLK)     P0_0            1
 *   SDA  (MOSI)    P0_6            9     (SPI_DO, see user_periph_setup.h)
 *   D/C            P0_5            7
 *   CS             P2_1            8     (SPI_EN, see user_periph_setup.h)
 *   RST            P0_7           10
 *   BUSY (input)   P2_0           40
 *   PWR-enable     P2_3           18    (driven high by the stock firmware)
 *   aux (unknown)  P2_2           13    (driven low by the stock firmware)
 *
 * SCK and MOSI here are the hardware SPI block's pads (see
 * user_periph_setup.h), shared with the boot flash.
 * ---------------------------------------------------------------------- */
#ifndef EPD_DC_PORT
#define EPD_DC_PORT      GPIO_PORT_0
#define EPD_DC_PIN       GPIO_PIN_5     /* QFN40 pin 7  */
#endif

#ifndef EPD_RST_PORT
#define EPD_RST_PORT     GPIO_PORT_0
#define EPD_RST_PIN      GPIO_PIN_7     /* QFN40 pin 10 */
#endif

#ifndef EPD_BUSY_PORT
#define EPD_BUSY_PORT    GPIO_PORT_2
#define EPD_BUSY_PIN     GPIO_PIN_0     /* QFN40 pin 40 */
#endif

/* Chip-select. Driven manually as a plain GPIO (the stock firmware bit-bangs
 * it too), NOT via the hardware SPI_EN function — so it must be configured
 * PID_GPIO. Physically confirmed on U4 connector pad 13. */
#ifndef EPD_CS_PORT
#define EPD_CS_PORT      GPIO_PORT_2
#define EPD_CS_PIN       GPIO_PIN_1     /* QFN40 pin 8  */
#endif

/* Panel power-enable line: the stock firmware drives P2_3 high at init.
 * Almost certainly gates the EPD's supply / booster — configure and assert
 * it or the panel may stay dark regardless of correct SPI. */
#ifndef EPD_PWR_PORT
#define EPD_PWR_PORT     GPIO_PORT_2
#define EPD_PWR_PIN      GPIO_PIN_3     /* QFN40 pin 18 */
#endif

#endif  /* board variant */

/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/** Configure DC/RST/BUSY GPIOs. Call once from set_pad_functions(). */
void epd_gpio_init(void);

/** Take the SPI bus back for the panel: restore D/C as a GPIO output and
 *  re-init the SPI master with the panel's settings. Call after anything else
 *  has driven the bus - the boot flash shares CLK/MOSI and claims P0_5, which
 *  is the panel's D/C line. */
void epd_spi_claim(void);

/** Bring the panel out of reset and run the SSD1680 init sequence.
 *  full_lut: true = full-refresh waveform, false = partial-refresh waveform
 *  (mirrors the two 30-byte LUT tables found in the reference firmware). */
void epd_init(bool full_lut);

/** Push a full 1bpp framebuffer (EPD_BUF_SIZE bytes, MSB-first per row,
 *  1 = white / 0 = black - matches the vendor's own canvas2bytes() packing,
 *  see PROTOCOL_NOTES.md section 6) into the panel's RAM and start a
 *  full-display refresh.
 *
 *  Returns as soon as the refresh is triggered - the panel then takes ~2 s on
 *  its own. Poll epd_display_busy() for completion; do NOT spin on it.
 *
 *  This is deliberately not a blocking call. The refresh outlasts the BLE
 *  supervision timeout, so blocking through one drops any open connection: a
 *  client could never stay connected for more than a minute (the clock's own
 *  minute tick refreshes), which breaks SUOTA and any multi-second transfer.
 *  Yielding between polls keeps the stack scheduled and the link alive. */
void epd_display_start(const uint8_t *framebuffer);

/** True while the panel is still refreshing. Poll from a timer, not a loop. */
bool epd_display_busy(void);

/** Put the panel into deep sleep to save power between refreshes. */
void epd_sleep(void);

#endif // _EPD_SSD1680_H_
