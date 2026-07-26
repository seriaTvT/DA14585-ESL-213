/**
 * epd_store.c - see epd_store.h.
 */

#include "epd_store.h"
#include "epd_ssd1680.h"
#include "user_periph_setup.h"
#include "spi.h"
#include "spi_flash.h"
#include "gpio.h"
#include "arch_wdg.h"

/* Last 4 KiB sector of the 256 KiB part.
 *
 * Deliberately outside everything the bootloader reads: SUOTA bank 1 is at
 * 0x002000, bank 2 at 0x014000 and the product header at 0x038000, so a
 * corrupt write here cannot cost us a bootable image. tools/mksuota.py blanks
 * this sector when it builds a flash image, so a freshly flashed tag starts
 * with no stored face and comes up on the built-in default. */
#define EPD_STORE_ADDR      0x03F000u
#define EPD_STORE_SECTOR    4096u
#define EPD_STORE_MAGIC     0x53445045u     /* "EPDS" */
#define EPD_STORE_MAX       1024u           /* matches CMD_SCRIPT_MAX */

/* Boot-flash pins. CLK/MOSI are the panel's too; P0_5 is the panel's D/C and
 * has to be handed back by epd_spi_claim() when we are done. */
#define FLASH_CS_PORT   GPIO_PORT_0
#define FLASH_CS_PIN    GPIO_PIN_3
#define FLASH_DI_PORT   GPIO_PORT_0
#define FLASH_DI_PIN    GPIO_PIN_5

#define FLASH_CHIP_SIZE (256 * 1024)

/* Header precedes the script in flash. Kept to 8 bytes so the whole record is
 * one page-aligned run for the page-program loop below. */
typedef struct {
    uint32_t magic;
    uint16_t len;
    uint16_t crc;
} store_hdr_t;

static const spi_cfg_t flash_spi_cfg = {
    .spi_ms      = SPI_MS_MODE_MASTER,
    .spi_cp      = SPI_CP_MODE_0,
    .spi_speed   = SPI_SPEED_MODE_4MHz,
    .spi_wsz     = SPI_MODE_8BIT,
    .spi_cs      = SPI_CS_0,
    .spi_irq     = SPI_IRQ_DISABLED,
    .cs_pad.port = FLASH_CS_PORT,
    .cs_pad.pin  = FLASH_CS_PIN,
};

static const spi_flash_cfg_t flash_cfg = {
    .chip_size = FLASH_CHIP_SIZE,
};

/* volatile so LTO keeps the stores: nothing in the firmware reads these, but
 * they are the only window into whether a save actually worked, read over SWD.
 * s_last_stage narrows an IO error to the operation that produced it. */
static volatile epd_store_res_t s_last_result = EPD_STORE_EMPTY;
static volatile uint32_t s_last_stage;      /* 1 acquire 2 erase 3 program
                                               4 readback 5 compare 9 done  */
static volatile uint32_t s_last_jedec;      /* JEDEC id seen on the bus      */
static uint8_t         s_buf[sizeof(store_hdr_t) + EPD_STORE_MAX];

/* CRC-16/CCITT. Only has to catch a torn or half-erased record, so the cheap
 * bitwise form is fine - this runs once per save, not per pixel. */
static uint16_t crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Take the bus for the flash: park the panel's chip select, turn its D/C line
 * into the flash's MISO, and re-init the SPI master for the flash. */
static bool flash_bus_acquire(void)
{
    uint8_t dev_id = 0;

    GPIO_ConfigurePin(EPD_CS_PORT, EPD_CS_PIN, OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(FLASH_CS_PORT, FLASH_CS_PIN, OUTPUT, PID_SPI_EN, true);

    /* Detach the panel's SPI_DI pad first. set_pad_functions() parks SPI_DI on
     * P0_2 (the panel is write-only and never uses it), and the flash needs
     * SPI_DI on P0_5. Two pads selecting the same peripheral input at once
     * gives a wired-OR of both, and the idle P0_2 wins - the flash then reads
     * back as all zeroes and even its JEDEC id comes out 0x000000. */
    GPIO_ConfigurePin(SPI_DI_PORT, SPI_DI_PIN, INPUT, PID_GPIO, false);
    GPIO_ConfigurePin(FLASH_DI_PORT, FLASH_DI_PIN, INPUT, PID_SPI_DI, false);

    spi_flash_configure_env(&flash_cfg);
    spi_initialize(&flash_spi_cfg);

    bool ok = spi_flash_enable_with_autodetect(&flash_spi_cfg, &dev_id)
              == SPI_FLASH_ERR_OK;

    /* Record what the bus actually answered. A sane JEDEC id here (0xA14013
     * on this part) but a failing write points at the write path; garbage
     * points at the bus hand-off from the panel. */
    uint32_t id = 0;
    spi_flash_read_jedec_id(&id);
    s_last_jedec = id;

    return ok;
}

static void flash_bus_release(void)
{
    /* Park the flash's chip select and give SPI_DI back to the panel's pad,
     * undoing exactly what flash_bus_acquire() changed. epd_spi_claim() then
     * restores P0_5 as D/C and re-inits the master for the panel. */
    GPIO_ConfigurePin(FLASH_CS_PORT, FLASH_CS_PIN, OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(SPI_DI_PORT, SPI_DI_PIN, INPUT, PID_SPI_DI, false);
    epd_spi_claim();
}

epd_store_res_t epd_store_save(const char *script, uint16_t len)
{
    store_hdr_t *hdr = (store_hdr_t *)s_buf;
    uint16_t total;
    epd_store_res_t res = EPD_STORE_OK;

    if (len > EPD_STORE_MAX) {
        s_last_result = EPD_STORE_BAD_LEN;
        return s_last_result;
    }

    hdr->magic = EPD_STORE_MAGIC;
    hdr->len   = len;
    hdr->crc   = crc16((const uint8_t *)script, len);
    for (uint16_t i = 0; i < len; i++) {
        s_buf[sizeof(store_hdr_t) + i] = (uint8_t)script[i];
    }
    total = (uint16_t)(sizeof(store_hdr_t) + len);

    s_last_stage = 1;
    if (!flash_bus_acquire()) {
        flash_bus_release();
        s_last_result = EPD_STORE_IO_ERR;
        return s_last_result;
    }

    /* Clear the block-protect bits before writing: with protection on, a page
     * program is accepted and quietly does nothing. */
    spi_flash_configure_memory_protection(SPI_FLASH_MEM_PROT_NONE);

    s_last_stage = 2;
    if (spi_flash_block_erase(EPD_STORE_ADDR, SPI_FLASH_OP_SE)
        != SPI_FLASH_ERR_OK) {
        res = EPD_STORE_IO_ERR;
    } else {
        s_last_stage = 3;
    }

    /* Page-program in 256-byte runs. spi_flash_write_data() wedges this part
     * (a Fudan FM25Q, JEDEC 0xA14013); the per-page call does not. */
    for (uint32_t done = 0; res == EPD_STORE_OK && done < total; ) {
        uint32_t at   = EPD_STORE_ADDR + done;
        uint32_t room = 256u - (at & 0xFFu);
        uint32_t n    = total - done;

        if (n > room) n = room;
        if (spi_flash_page_program(s_buf + done, at, (uint16_t)n)
            != SPI_FLASH_ERR_OK) {
            res = EPD_STORE_IO_ERR;
            break;
        }
        wdg_reload(0xFF);
        done += n;
    }

    /* Read back rather than trusting the return codes. This part has been seen
     * to report a clean page program that never landed, so an unverified save
     * would be worse than none - it would look like persistence and silently
     * lose the face at the next power cycle. */
    if (res == EPD_STORE_OK) {
        uint32_t got = 0;
        static uint8_t vbuf[sizeof(store_hdr_t) + EPD_STORE_MAX];

        s_last_stage = 4;
        if (spi_flash_read_data(vbuf, EPD_STORE_ADDR, total, &got)
                != SPI_FLASH_ERR_OK || got != total) {
            res = EPD_STORE_IO_ERR;
        } else {
            s_last_stage = 5;
            for (uint16_t i = 0; i < total; i++) {
                if (vbuf[i] != s_buf[i]) {
                    res = EPD_STORE_VERIFY_ERR;
                    break;
                }
            }
        }
    }

    flash_bus_release();
    if (res == EPD_STORE_OK) s_last_stage = 9;
    s_last_result = res;
    return res;
}

epd_store_res_t epd_store_load(char *out, uint16_t out_size, uint16_t *out_len)
{
    store_hdr_t hdr;
    uint32_t got = 0;
    epd_store_res_t res = EPD_STORE_OK;

    *out_len = 0;

    if (!flash_bus_acquire()) {
        flash_bus_release();
        return EPD_STORE_IO_ERR;
    }

    if (spi_flash_read_data((uint8_t *)&hdr, EPD_STORE_ADDR, sizeof(hdr), &got)
            != SPI_FLASH_ERR_OK || got != sizeof(hdr)) {
        res = EPD_STORE_IO_ERR;
    } else if (hdr.magic == 0xFFFFFFFFu) {
        res = EPD_STORE_EMPTY;          /* erased sector - nothing saved yet */
    } else if (hdr.magic != EPD_STORE_MAGIC) {
        res = EPD_STORE_BAD_MAGIC;
    } else if (hdr.len == 0 || hdr.len > EPD_STORE_MAX || hdr.len > out_size) {
        res = EPD_STORE_BAD_LEN;
    } else if (spi_flash_read_data((uint8_t *)out,
                                   EPD_STORE_ADDR + sizeof(hdr),
                                   hdr.len, &got) != SPI_FLASH_ERR_OK
               || got != hdr.len) {
        res = EPD_STORE_IO_ERR;
    } else if (crc16((const uint8_t *)out, hdr.len) != hdr.crc) {
        res = EPD_STORE_BAD_CRC;
    } else {
        *out_len = hdr.len;
    }

    flash_bus_release();
    return res;
}

epd_store_res_t epd_store_last_result(void)
{
    return s_last_result;
}
