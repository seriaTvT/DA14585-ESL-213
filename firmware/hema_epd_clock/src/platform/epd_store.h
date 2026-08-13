/**
 * epd_store.h - persist the display template in SPI flash.
 *
 * Without this the tag forgets its face on every power cycle and falls back to
 * the built-in default, so a host has to re-push its template after each power
 * loss. The clock itself still cannot survive a power cycle (there is no RTC -
 * see epd_time.h), so a host is expected to re-sync the time on connect; the
 * point here is only that it should not have to re-send the *picture*.
 *
 * Stored in one 4 KiB sector well clear of the SUOTA image banks and the
 * product header, so a firmware update does not disturb it and a bad write
 * here cannot corrupt a bootable image - see EPD_STORE_ADDR in epd_store.c.
 *
 * Bus note: the flash shares CLK/MOSI with the panel and takes over P0_5,
 * which is the panel's D/C. Both calls below acquire the bus, use it, and hand
 * it straight back with epd_spi_claim(), so callers do not have to care - but
 * they must not be called while a panel transfer is in flight.
 */

#ifndef _EPD_STORE_H_
#define _EPD_STORE_H_

#include <stdint.h>
#include <stdbool.h>

/** Result of the last epd_store_save()/epd_store_load(), for diagnostics.
 *
 * Values are pinned, and new ones are appended rather than slotted in where
 * they read best. These are looked up by eye against this header while
 * squinting at `s_last_result` over SWD, so a note written during one bring-up
 * session has to still mean the same thing during the next one. */
typedef enum {
    EPD_STORE_OK          = 0,
    EPD_STORE_EMPTY       = 1,  /* nothing stored yet (blank sector)       */
    EPD_STORE_BAD_MAGIC   = 2,
    EPD_STORE_BAD_LEN     = 3,
    EPD_STORE_BAD_CRC     = 4,
    EPD_STORE_IO_ERR      = 5,  /* the flash driver reported a failure     */
    EPD_STORE_VERIFY_ERR  = 6,  /* written, but the read-back disagreed    */
    EPD_STORE_BAD_VERSION = 7,  /* a face written by an older DSL revision */
} epd_store_res_t;

/** Write `script`/`len` to flash, then read it back and check it landed.
 *  Erases the sector first. Returns EPD_STORE_OK only if the read-back
 *  matches - a silent write failure on this part is a real failure mode, so
 *  success is never assumed from the driver's return code alone. */
epd_store_res_t epd_store_save(const char *script, uint16_t len);

/** Load a stored template into `out` (capacity `out_size`). On anything other
 *  than EPD_STORE_OK, `*out_len` is 0 and the caller should fall back to the
 *  built-in face. */
epd_store_res_t epd_store_load(char *out, uint16_t out_size, uint16_t *out_len);

/* No epd_store_last_result() / epd_store_last_load() accessors. The results are
 * still recorded, in the file-static `s_last_result` and `s_last_load`, and
 * they are still what you want at boot - a stale (BAD_VERSION) or corrupt
 * (BAD_CRC) face is invisible otherwise, since either simply brings the tag up
 * on the built-in default. They are kept apart on purpose: one variable holding
 * whichever happened last cannot say which it was.
 *
 * The accessors went on 2026-08-13 because nothing called them and nothing
 * could usefully call them. They existed "for SWD bring-up", but tools/tagread.py
 * reads VARIABLES out of the ELF by address - it cannot call a function - and
 * both statics are volatile and present in the symbol table, so the SWD route
 * never went through here. Read `s_last_result` / `s_last_load` directly. */

/** Hold the flash bus across many operations.
 *
 * Was SUOTA-only until epd_board_read() needed the same hand-off to read the
 * board record at 0x039000. Unconditional now, because duplicating the acquire
 * would mean describing the pad-detach order in two places, which is exactly
 * what the wrappers exist to avoid.
 *
 * The two calls above are self-contained: each takes the bus and hands it back.
 * A SUOTA session cannot work that way. It writes the image in ~230 blocks over
 * minutes, the SDK's receiver re-derives the pin configuration on every block
 * from a map the *client* sent, and between blocks the kernel scheduler runs -
 * which is where a minute tick would otherwise call epd_spi_claim() and take
 * the bus back mid-image.
 *
 * So the session claims the bus once at SUOTAR_START and releases it at
 * SUOTAR_END, and the app suppresses repaints in between. Panel and flash
 * cannot share the bus; on variant B they share CLK and MOSI outright and P0_5
 * is the panel's D/C *and* the flash's MISO, so "share" is not available as a
 * design.
 *
 * epd_store_flash_claim() returns false if the flash did not answer, in which
 * case the session should be refused rather than written blind.
 *
 * Callers must pair these. epd_store_flash_release() hands the bus back to the
 * panel via epd_spi_claim(), so a missed release leaves the panel mute until
 * the next reboot - the panel's pins are still configured for the flash. */
bool epd_store_flash_claim(void);
void epd_store_flash_release(void);

#endif // _EPD_STORE_H_
