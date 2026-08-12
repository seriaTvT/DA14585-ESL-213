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

    return epd_board_decode(rec, out);
}
