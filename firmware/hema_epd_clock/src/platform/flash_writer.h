/**
 * flash_writer.h - SWD-driven SPI flash programmer.
 *
 * Why this exists: the DA14585 has no internal flash, so our firmware has to
 * live in the external SPI flash (U3) to survive a power cycle. J-Link can't
 * write it - that flash hangs off the chip's SPI pins, so there is no J-Link
 * flash bank for it (verified: `-device DA14585` exposes only the "Default"
 * memory zone). The SDK ships utilities/flash_programmer for exactly this, but
 * it is a Keil-only project and its default transport is UART, which this tag
 * doesn't expose - only SWD is soldered.
 *
 * So instead this reuses our own already-linked spi_flash.c/spi_58x.c: build
 * the firmware with EPD_FLASH_WRITER 1, RAM-load it over SWD as usual, and it
 * runs a mailbox loop instead of the BLE app. The host drives it by poking a
 * struct in RAM with J-Link and polling for completion - the same trick
 * SmartSnippets uses, just with a protocol we control.
 *
 * Protocol: host fills addr/len/data, then writes `cmd` last. The target sets
 * status = FW_ST_BUSY, runs the command, sets status, and clears `cmd` last.
 * So the host polls until cmd == 0, then reads status. Writing `cmd` last on
 * one side and clearing it last on the other is what makes the handshake safe
 * without any other synchronisation.
 */

#ifndef _FLASH_WRITER_H_
#define _FLASH_WRITER_H_

/* Set to 1 to build the flasher instead of the BLE clock app. Kept as a
 * header #define rather than a -D so the headless `make` needs no extra
 * flags; tools/flash_image.py flips it and puts it back. */
#define EPD_FLASH_WRITER   0

#include <stdint.h>

#define FW_MAGIC        0x464C5357u   /* "FLSW" - proves the stub is live */
#define FW_CHUNK        4096u         /* one sector per round trip */

/* commands (host -> target) */
#define FW_CMD_NONE     0u
#define FW_CMD_INFO     1u            /* JEDEC id -> info */
#define FW_CMD_ERASE    2u            /* whole chip */
#define FW_CMD_WRITE    3u            /* data[0..len) -> addr */
#define FW_CMD_READ     4u            /* addr -> data[0..len) */
#define FW_CMD_SECTOR   5u            /* erase the sector holding addr */
#define FW_CMD_PAGE     6u            /* single page program, diagnostic */
#define FW_CMD_STRESS   7u            /* loop erase+program forever, for a meter */

/* status (target -> host) */
#define FW_ST_BUSY      0u
#define FW_ST_OK        1u
#define FW_ST_ERR       2u

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t cmd;
    volatile uint32_t addr;
    volatile uint32_t len;
    volatile uint32_t status;
    volatile uint32_t info;     /* JEDEC id, or driver error code on FW_ST_ERR */
    volatile uint8_t  data[FW_CHUNK];
} fw_mailbox_t;

/* Deliberately non-static: the host finds it by name in hema_epd_clock.map. */
extern fw_mailbox_t fw_mb;

/** Run the mailbox loop. Never returns. */
void flash_writer_main(void);

#endif // _FLASH_WRITER_H_
