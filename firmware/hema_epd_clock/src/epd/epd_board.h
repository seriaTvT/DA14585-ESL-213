/**
 ****************************************************************************************
 *
 * @file epd_board.h
 *
 * @brief What board this actually is, read off the tag instead of assumed.
 *
 * The vendor ships ONE image for every board and lets each tag say what it is.
 * The mechanism was recovered 2026-08-12 and is written up in
 * hema-local/docs/TAG_VARIANTS.md; the short version is that a 16-byte record
 * at flash 0x039000 carries both axes:
 *
 *     +0x00        panel model     0x14 = A53 122x250, 0x09 = A41 104x212
 *     +0x01        pin-map select  0x01 = use the map below, 0xFF = built-in
 *     +0x08..0x0F  the pin map, one byte per entry, packed (port << 4) | pin
 *
 * and that a board needing no override simply leaves the record erased. The
 * vendor's built-in default is **variant B**, so an erased record means
 * variant B, and only a variant-A board carries a written map. Both of our
 * variant-B tags (Types 1 and 4) have it erased; the variant-A tag (Type 3)
 * carries `21 22 10 01 20 07 11 23`, which unpacks to exactly the pin table
 * its own firmware builds in RAM.
 *
 * That record is the thing to trust when identifying an unfamiliar tag. It is
 * also the only safe way to tell the variants apart: the two pin maps overlap
 * on P1_1 with OPPOSITE directions - variant A has it as BUSY, a panel output,
 * where variant B drives it as an enable - so probing by driving pins risks
 * contention against the panel. Reading flash costs nothing and risks nothing.
 *
 * WHAT THIS IS FOR TODAY. The firmware still selects its wiring and geometry
 * at build time (see config/tag_types.h). This module reads what the board
 * says so the two can be COMPARED - a wrong-variant image is otherwise silent,
 * booting and advertising normally with only a dead panel to show for it, which
 * reads as broken hardware rather than a bad flash. It has cost a working tag
 * twice. Making the driver consume this instead of the macros is the next step
 * and a larger one: the variants differ in more than pin numbers (variant A
 * bit-bangs where B uses the hardware SPI block it shares with the boot flash),
 * and the geometry sizes the framebuffer.
 *
 ****************************************************************************************
 */

#ifndef _EPD_BOARD_H_
#define _EPD_BOARD_H_

#include <stdbool.h>
#include <stdint.h>

/** Where the record lives, and how much of it matters. */
#define EPD_BOARD_REC_ADDR      0x039000
#define EPD_BOARD_REC_LEN       16

/** Entry order within the packed map. Taken from the vendor's own runtime
 *  table, which our driver's names map onto one for one. Slot 1 is the odd one
 *  out: variant A holds it high and variant B's table names a different pin
 *  there than our build does, so treat it as "the vendor's aux" rather than as
 *  a signal with an agreed meaning. */
typedef enum {
    EPD_BOARD_CS = 0,
    EPD_BOARD_AUX,
    EPD_BOARD_RST,
    EPD_BOARD_SCK,
    EPD_BOARD_SDA,
    EPD_BOARD_DC,
    EPD_BOARD_BUSY,
    EPD_BOARD_PWR,
    EPD_BOARD_NPINS
} epd_board_signal_t;

typedef enum {
    EPD_BOARD_PANEL_UNKNOWN = 0,
    EPD_BOARD_PANEL_A53,        /**< 122x250 */
    EPD_BOARD_PANEL_A41         /**< 104x212 */
} epd_board_panel_t;

typedef struct {
    epd_board_panel_t panel;
    /** false when the record is erased or malformed, which is not an error:
     *  it is how a variant-B board says "the built-in default suits me". */
    bool              have_pinmap;
    uint8_t           port[EPD_BOARD_NPINS];
    uint8_t           pin[EPD_BOARD_NPINS];
} epd_board_t;

/**
 * @brief Decode a 16-byte board record. Pure; no hardware, no side effects.
 *
 * Always fills @p out. An erased or malformed record yields
 * have_pinmap = false and panel = UNKNOWN rather than a failure, because a
 * blank record is a legitimate state that means "use the defaults".
 *
 * @param[in]  rec  EPD_BOARD_REC_LEN bytes read from EPD_BOARD_REC_ADDR
 * @param[out] out  decoded result
 * @return true if a usable pin map was found
 */
bool epd_board_decode(const uint8_t *rec, epd_board_t *out);

/**
 * @brief Read the record off the boot flash and decode it.
 *
 * Takes and releases the flash bus itself. Returns false and leaves @p out at
 * its defaults if the read fails.
 */
bool epd_board_read(epd_board_t *out);

/** What the boot-time check concluded. Readable over SWD for bring-up, the
 *  same way epd_store_last_load() is. */
typedef enum {
    EPD_BOARD_UNCHECKED = 0,    /**< epd_board_check() has not run           */
    EPD_BOARD_AGREES,           /**< record matches this build, or is blank  */
    EPD_BOARD_UNREADABLE,       /**< the flash read failed - not a mismatch  */
    EPD_BOARD_MISMATCH          /**< the board says it is the other variant  */
} epd_board_verdict_t;

/**
 * @brief Read the record and record what it says about this build. Once, at boot.
 *
 * Never changes behaviour on its own. Whether a mismatch stops the panel being
 * driven is EPD_BOARD_CHECK's business, and that defaults to off - see
 * user_periph_setup.c for why.
 */
void epd_board_check(void);

/** The verdict from epd_board_check(), and the record it was based on. */
epd_board_verdict_t epd_board_verdict(void);
const epd_board_t *epd_board_last(void);

/**
 * @brief Does the board agree with what this image was built for?
 *
 * Compares the decoded record against EPD_BOARD_VARIANT_A/B and
 * EPD_PANEL_LOW_RES. A board whose record is erased claims variant B.
 * UNKNOWN panel or absent pin map means "cannot tell", which counts as
 * agreement - never refuse on the strength of a blank record.
 */
bool epd_board_matches_build(const epd_board_t *b);

#endif /* _EPD_BOARD_H_ */
