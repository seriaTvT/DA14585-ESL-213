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

/* Last 4 KiB sector of the first 256 KiB.
 *
 * Deliberately outside everything the bootloader reads: SUOTA bank 1 is at
 * 0x002000, bank 2 at 0x014000 and the product header at 0x038000, so a
 * corrupt write here cannot cost us a bootable image. tools/mksuota.py blanks
 * this sector when it builds a flash image, so a freshly flashed tag starts
 * with no stored face and comes up on the built-in default.
 *
 * The parts we have measured are 512 KiB, so this is *not* the last sector of
 * the chip, and it deliberately stays where it is. Moving it to 0x07F000 would
 * orphan the face on every tag already in the field, and would brick the store
 * outright on any variant whose flash turns out to be 256 KiB - Type 3's is
 * still unmeasured. 0x03F000 is the one address that works on both. The
 * 0x040000-0x080000 half is untouched and free for anything that wants it. */
#define EPD_STORE_ADDR      0x03F000u
#define EPD_STORE_SECTOR    4096u
/* "EPD2". Bumped once from "EPDS" when the DSL was redesigned: the old records
 * have no version field, so there is nothing in them to compare against, and
 * reinterpreting their bytes under the new layout would read the old `len` as
 * a version. Rejecting them on the magic is unambiguous instead of nearly
 * always right. Records written from here on carry EPD_STORE_VERSION, so this
 * value should not need to change again. */
#define EPD_STORE_MAGIC     0x32445045u     /* "EPD2" */
/* Revision of the *language* the stored script is written in, as opposed to
 * the revision of this container. A face is never migrated on the tag - there
 * is nowhere to rewrite 1 KB of text on a Cortex-M0 with this little RAM, and
 * the host can always re-push - so a mismatch simply falls back to the
 * built-in default face. Bump this on any breaking DSL change. */
/* 2: FONT() became TEXT(), and ROTATE() takes degrees only.
 *
 * Both are silent breakages rather than loud ones, which is exactly what this
 * field is for. A stored FONT() line would simply stop drawing. Worse,
 * ROTATE(3) used to mean 270 degrees and now means an unsupported 3 - so a
 * landscape face restored from flash under the new parser would come back
 * portrait, on a tag nobody is holding, with no host in range to notice. */
#define EPD_STORE_VERSION   2u
#define EPD_STORE_MAX       3072u           /* matches CMD_SCRIPT_MAX */

/* Boot-flash pins. CLK/MOSI are the panel's too; P0_5 is the panel's D/C and
 * has to be handed back by epd_spi_claim() when we are done. */
#define FLASH_CS_PORT   GPIO_PORT_0
#define FLASH_CS_PIN    GPIO_PIN_3
#define FLASH_DI_PORT   GPIO_PORT_0
#define FLASH_DI_PIN    GPIO_PIN_5

/* Fallback size, used until the JEDEC id says otherwise.
 *
 * 256 KiB rather than the 512 KiB actually fitted, because chip_size is only
 * ever used by the SDK as a clamp - `if (size > chip_size - address)` - so
 * guessing low costs nothing while guessing high would let a write run off the
 * end of a smaller part. It has to be at least 0x40000 for the store sector at
 * 0x03F000 to be reachable at all, so this is also the smallest safe value. */
#define FLASH_CHIP_SIZE_FALLBACK    (256u * 1024u)

/* Capacity byte range we will believe, as a power of two: 64 KiB to 16 MiB.
 * Anything outside that is a misread rather than a real part. */
#define FLASH_CAP_SHIFT_MIN 16u
#define FLASH_CAP_SHIFT_MAX 24u

/* Third JEDEC byte is log2 of the capacity in bytes: 0x13 -> 512 KiB, which is
 * what the 0xA14013 part fitted to every tag we have measured reports.
 *
 * Needed because spi_flash_auto_detect() only knows the parts in the SDK's own
 * table and ours is not one of them - it returns SPI_FLASH_ERR_NOT_DETECTED and
 * leaves chip_size at whatever was configured before. Deriving it here keeps us
 * correct on tag variants whose flash nobody has measured yet, instead of
 * carrying a hardcoded number that happens to be right for two boards. */
static uint32_t flash_size_from_jedec(uint32_t jedec_id)
{
    uint32_t shift = jedec_id & 0xFFu;

    if (shift < FLASH_CAP_SHIFT_MIN || shift > FLASH_CAP_SHIFT_MAX) {
        return FLASH_CHIP_SIZE_FALLBACK;
    }
    return 1u << shift;
}

/* Header precedes the script in flash. `reserved` is explicit rather than left
 * to the compiler: the struct is written to flash byte for byte and read back
 * and compared byte for byte, so an implicit padding hole would be comparing
 * whatever the buffer happened to hold. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint16_t crc;
    uint16_t reserved;
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
    .chip_size = FLASH_CHIP_SIZE_FALLBACK,
};

/* volatile so LTO keeps the stores: nothing in the firmware reads these, but
 * they are the only window into whether a save actually worked, read over SWD.
 * s_last_stage narrows an IO error to the operation that produced it. */
static volatile epd_store_res_t s_last_result = EPD_STORE_EMPTY;
/* Kept apart from s_last_result rather than sharing it: a load and a save
 * answer different questions, and one variable holding whichever happened most
 * recently cannot say which it was. The load result is the more useful of the
 * two at boot, since that is where a stale or corrupt face shows up - and
 * EPD_STORE_BAD_VERSION exists precisely to be read here. */
static volatile epd_store_res_t s_last_load = EPD_STORE_EMPTY;
static volatile uint32_t s_last_stage;      /* 1 acquire 2 erase 3 program
                                               4 readback 5 compare 9 done  */
static volatile uint32_t s_last_jedec;      /* JEDEC id seen on the bus      */
static volatile uint32_t s_last_chip_size;  /* size the SDK ended up clamping
                                               to, derived or fallback        */
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

    /* Ask the driver to park its own chip select rather than naming the pin
     * here. CS moves with the board now, and a second copy of the map in this
     * file would eventually disagree with the real one - leaving the panel
     * SELECTED for the whole flash transaction, hearing flash traffic as
     * commands, with nothing to report it. */
    epd_cs_park();
    GPIO_ConfigurePin(FLASH_CS_PORT, FLASH_CS_PIN, OUTPUT, PID_SPI_EN, true);

    /* Detach the panel's SPI_DI pad first. set_pad_functions() parks SPI_DI on
     * P0_2 (the panel is write-only and never uses it), and the flash needs
     * SPI_DI on P0_5. Two pads selecting the same peripheral input at once
     * gives a wired-OR of both, and the idle P0_2 wins - the flash then reads
     * back as all zeroes and even its JEDEC id comes out 0x000000. */
    GPIO_ConfigurePin(SPI_DI_PORT, SPI_DI_PIN, INPUT, PID_GPIO, false);
    GPIO_ConfigurePin(FLASH_DI_PORT, FLASH_DI_PIN, INPUT, PID_SPI_DI, false);

    /* Clock and data back to the SPI block.
     *
     * Only variant B needs this, and only since the panel started bit-banging:
     * these two pads are shared, and the panel leaves them as plain GPIOs when
     * it is done (epd_spi_claim()). Without this the flash gets a clock line
     * the SPI block cannot drive, and every transaction reads back as zeroes -
     * the same symptom as the SPI_DI collision above and just as quiet.
     *
     * Unconditional because on variant A it is a no-op that restates the truth:
     * set_pad_functions() already put these pads here and nothing on that board
     * ever moves them. Cheaper than a conditional, and it means this function
     * establishes what it needs rather than inheriting it. */
    GPIO_ConfigurePin(SPI_CLK_PORT, SPI_CLK_PIN, OUTPUT, PID_SPI_CLK, false);
    GPIO_ConfigurePin(SPI_DO_PORT,  SPI_DO_PIN,  OUTPUT, PID_SPI_DO,  false);

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

    /* dev_id is left at 0 when the part is not in the SDK's table, which is our
     * case. Only then do we override the size: a recognised part keeps the
     * env the SDK just wrote, because dev_index is not merely informational -
     * spi_flash_configure_memory_protection() branches on it for the
     * AT25xy021A - and writing our own cfg over it would zero that.
     *
     * Note spi_flash_enable_with_autodetect() cannot tell us this through its
     * return value: it assigns the autodetect result to `status` and then
     * overwrites it with the memory-protection result before returning, so a
     * part that is not in the table still reports SPI_FLASH_ERR_OK. dev_id is
     * the only honest signal, which is why `ok` above is not enough. */
    s_last_chip_size = FLASH_CHIP_SIZE_FALLBACK;
    if (ok && dev_id == 0) {
        spi_flash_cfg_t cfg = {
            .dev_index = 0,
            .jedec_id  = id,
            .chip_size = flash_size_from_jedec(id),
        };
        spi_flash_configure_env(&cfg);
        s_last_chip_size = cfg.chip_size;
    }

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

    hdr->magic    = EPD_STORE_MAGIC;
    hdr->version  = EPD_STORE_VERSION;
    hdr->len      = len;
    hdr->crc      = crc16((const uint8_t *)script, len);
    hdr->reserved = 0;
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
        s_last_load = EPD_STORE_IO_ERR;
        return s_last_load;
    }

    if (spi_flash_read_data((uint8_t *)&hdr, EPD_STORE_ADDR, sizeof(hdr), &got)
            != SPI_FLASH_ERR_OK || got != sizeof(hdr)) {
        res = EPD_STORE_IO_ERR;
    } else if (hdr.magic == 0xFFFFFFFFu) {
        res = EPD_STORE_EMPTY;          /* erased sector - nothing saved yet */
    } else if (hdr.magic != EPD_STORE_MAGIC) {
        res = EPD_STORE_BAD_MAGIC;
    } else if (hdr.version != EPD_STORE_VERSION) {
        /* A face written by an older language. Not an error in any useful
         * sense - the tag comes up on the built-in default and the host
         * re-pushes - but distinguished from corruption so that bring-up can
         * tell "this tag has an old face" from "this flash is failing". */
        res = EPD_STORE_BAD_VERSION;
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
    s_last_load = res;
    return res;
}

/* Thin wrappers rather than making the statics non-static, so that the bus
 * hand-off - the pad detach order, the JEDEC size override, the fact that
 * release goes through epd_spi_claim() - stays described in exactly one place.
 * SUOTA needs the same acquire the store needs; what differs is only how long
 * it holds it. epd_board_read() needs it too, for one 16-byte read at boot.
 * See the header for why it cannot be per-operation. */
bool epd_store_flash_claim(void)
{
    return flash_bus_acquire();
}

void epd_store_flash_release(void)
{
    flash_bus_release();
}
