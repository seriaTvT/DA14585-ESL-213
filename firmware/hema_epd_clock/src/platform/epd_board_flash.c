/**
 ****************************************************************************************
 *
 * @file epd_board_flash.c
 *
 * @brief Getting the board record off the boot flash. See epd/epd_board.h.
 *
 * Split from epd_board.c so the decoder stays pure C and can be run on the
 * host against bytes taken from real tags (test/test_board.c). Everything
 * here is the hardware half: sixteen bytes, once, at boot.
 *
 ****************************************************************************************
 */

#include "epd_board.h"
#include "epd_store.h"
#include "spi_flash.h"

/* The boot-time verdict, kept here rather than in epd_board.c so that the
 * decoder stays pure and host-testable. */
static epd_board_verdict_t s_verdict = EPD_BOARD_UNCHECKED;
static epd_board_t         s_board;

bool epd_board_read(epd_board_t *out)
{
    uint8_t rec[EPD_BOARD_REC_LEN];
    uint32_t got = 0;
    bool ok;

    /* Decode a NULL record first, so `out` is populated with the defaults even
     * if the flash never answers. A tag whose flash is unreadable has bigger
     * problems than its pin map, but the caller should not have to care about
     * the order it checks things in. */
    (void)epd_board_decode(0, out);

    if (!epd_store_flash_claim()) {
        return false;
    }

    ok = (spi_flash_read_data(rec, EPD_BOARD_REC_ADDR, sizeof(rec), &got)
              == SPI_FLASH_ERR_OK)
         && got == sizeof(rec);

    epd_store_flash_release();

    if (!ok) {
        return false;
    }

    /* The READ is what this function reports, not what the record contained.
     *
     * epd_board_decode() answers a different question - "did I find an explicit
     * pin map?" - and says false for an ERASED record, which is not a failure
     * at all: it is how a variant-B board states that the built-in map suits
     * it. Returning that here conflated the two, and epd_board_check() turned
     * it into EPD_BOARD_UNREADABLE.
     *
     * That was invisible while nothing acted on the verdict. The moment an
     * unreadable board stopped being driven, it meant EVERY variant-B tag
     * refused its own panel - Types 1 and 4 both carry 0xFF in the selector
     * byte - while variant A worked, because only variant A carries a map.
     * Caught on a Type 1; it would have shipped otherwise.
     *
     * Whether a map was found is still available, and is the honest place to
     * ask: out->have_pinmap. */
    (void)epd_board_decode(rec, out);
    return true;
}

void epd_board_check(void)
{
    if (!epd_board_read(&s_board)) {
        /* Distinguished from a mismatch on purpose. A flash that will not
         * answer says nothing about which board this is, and treating silence
         * as disagreement would refuse the panel on a tag whose only fault is
         * a flaky read. */
        s_verdict = EPD_BOARD_UNREADABLE;
        return;
    }

    s_verdict = EPD_BOARD_AGREES;
}

epd_board_verdict_t epd_board_verdict(void)
{
    return s_verdict;
}

const epd_board_t *epd_board_last(void)
{
    return &s_board;
}
