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

/** Result of the last epd_store_save()/epd_store_load(), for diagnostics. */
typedef enum {
    EPD_STORE_OK = 0,
    EPD_STORE_EMPTY,        /* nothing stored yet (blank sector)      */
    EPD_STORE_BAD_MAGIC,
    EPD_STORE_BAD_LEN,
    EPD_STORE_BAD_CRC,
    EPD_STORE_IO_ERR,       /* the flash driver reported a failure    */
    EPD_STORE_VERIFY_ERR,   /* written, but the read-back disagreed   */
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

/** Result of the most recent save, readable over SWD for bring-up. */
epd_store_res_t epd_store_last_result(void);

#endif // _EPD_STORE_H_
