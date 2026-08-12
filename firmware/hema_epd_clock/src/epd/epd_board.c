/**
 ****************************************************************************************
 *
 * @file epd_board.c
 *
 * @brief Decoding the board record. Pure C - see epd_board.h for what it is.
 *
 * Kept free of hardware so it can be exercised on the host against bytes taken
 * from real tags (test/test_board.c). The flash read that feeds it lives in
 * platform/epd_board_flash.c.
 *
 ****************************************************************************************
 */

#include "epd_board.h"

/* The vendor's built-in default, which is what an erased record selects. These
 * are the sixteen constants its own code stores into its runtime pin table, in
 * this module's signal order. Kept here rather than derived from the
 * EPD_*_PORT macros in epd_ssd1680.h on purpose: this is a statement about what
 * the VENDOR does, and it has to stay true even if our build's idea of variant
 * B changes. The two disagree on AUX today - the vendor names P1_1 there where
 * our variant-B build drives P2_2 - and hiding that behind a shared macro would
 * lose exactly the discrepancy worth noticing. */
static const uint8_t k_default_packed[EPD_BOARD_NPINS] = {
    0x21,   /* CS   P2_1 */
    0x11,   /* AUX  P1_1 */
    0x07,   /* RST  P0_7 */
    0x00,   /* SCK  P0_0 */
    0x06,   /* SDA  P0_6 */
    0x05,   /* DC   P0_5 */
    0x20,   /* BUSY P2_0 */
    0x23    /* PWR  P2_3 */
};

/* Ports run 0..3. Pin counts differ per port on this part - P0 and P3 have 8,
 * P1 has 6, P2 has 10 - and a byte that decodes to a pin the port does not have
 * means the record is not what we think it is, so check rather than clamp. */
static const uint8_t k_port_pins[4] = { 8, 6, 10, 8 };

static bool packed_ok(uint8_t b)
{
    uint8_t port = (uint8_t)(b >> 4);
    uint8_t pin  = (uint8_t)(b & 0x0F);

    return port < 4 && pin < k_port_pins[port];
}

static void unpack(const uint8_t *packed, epd_board_t *out)
{
    int i;

    for (i = 0; i < EPD_BOARD_NPINS; i++) {
        out->port[i] = (uint8_t)(packed[i] >> 4);
        out->pin[i]  = (uint8_t)(packed[i] & 0x0F);
    }
}

bool epd_board_decode(const uint8_t *rec, epd_board_t *out)
{
    int i;

    /* Default first, so every path leaves `out` fully populated. An erased
     * record is the common case, not an error - it is how a variant-B board
     * says the built-in map suits it. */
    out->panel       = EPD_BOARD_PANEL_UNKNOWN;
    out->have_pinmap = false;
    unpack(k_default_packed, out);

    if (rec == 0) {
        return false;
    }

    switch (rec[0]) {
    case 0x14: out->panel = EPD_BOARD_PANEL_A53; break;
    case 0x09: out->panel = EPD_BOARD_PANEL_A41; break;
    default:   out->panel = EPD_BOARD_PANEL_UNKNOWN; break;
    }

    /* Only the one selector value means "a map follows". Anything else -
     * 0xFF erased, 0x00, or a value from a revision we have not seen - falls
     * back to the default, which is what the vendor's own code does: it tests
     * for 1 and takes the default branch otherwise. */
    if (rec[1] != 0x01) {
        return false;
    }

    for (i = 0; i < EPD_BOARD_NPINS; i++) {
        if (!packed_ok(rec[8 + i])) {
            /* Selector said there is a map and there is not one. Keep the
             * default rather than half-applying it. */
            return false;
        }
    }

    unpack(&rec[8], out);
    out->have_pinmap = true;
    return true;
}

/* The build's own idea of itself, so the comparison below is against what this
 * image will actually do rather than against a number someone typed. */
#if defined(EPD_BOARD_VARIANT_A)
    #define BUILD_IS_VARIANT_A  1
#else
    #define BUILD_IS_VARIANT_A  0
#endif

#if defined(EPD_PANEL_LOW_RES)
    #define BUILD_PANEL         EPD_BOARD_PANEL_A41
#else
    #define BUILD_PANEL         EPD_BOARD_PANEL_A53
#endif

bool epd_board_matches_build(const epd_board_t *b)
{
    /* A written map means variant A: variant B is the vendor's default and
     * never carries one. That is an inference from three tags, not from the
     * code, so it is stated here where it can be found rather than buried. */
    bool board_is_variant_a = b->have_pinmap;

    if (board_is_variant_a != (BUILD_IS_VARIANT_A != 0)) {
        return false;
    }

    /* An unreadable panel byte is not evidence of a mismatch. Refusing on a
     * blank record would brick bring-up on any tag whose record we have not
     * seen, which is exactly the tag most likely to need bring-up. */
    if (b->panel != EPD_BOARD_PANEL_UNKNOWN && b->panel != BUILD_PANEL) {
        return false;
    }

    return true;
}
