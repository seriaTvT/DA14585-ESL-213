/**
 * flash_writer.c - see flash_writer.h.
 */

#include "flash_writer.h"

#if EPD_FLASH_WRITER

#include "spi.h"
#include "spi_flash.h"
#include "gpio.h"
#include "datasheet.h"

/* Boot-flash pins (U3), recovered from the stock firmware and confirmed by
 * continuity test - see PROTOCOL_NOTES.md section 13. Note P0_5 is shared:
 * it is the flash's MISO *and* the EPD's D/C, time-shared via separate chip
 * selects. This build never touches the EPD, so the conflict can't bite. */
#define FLASH_CS_PORT    GPIO_PORT_0
#define FLASH_CS_PIN     GPIO_PIN_3
#define FLASH_CLK_PORT   GPIO_PORT_0
#define FLASH_CLK_PIN    GPIO_PIN_0
#define FLASH_DO_PORT    GPIO_PORT_0
#define FLASH_DO_PIN     GPIO_PIN_6
#define FLASH_DI_PORT    GPIO_PORT_0
#define FLASH_DI_PIN     GPIO_PIN_5

#define FLASH_CHIP_SIZE  (256 * 1024)   /* matches the stock image's size */

static const spi_cfg_t flash_spi_cfg = {
    .spi_ms      = SPI_MS_MODE_MASTER,
    .spi_cp      = SPI_CP_MODE_0,
    .spi_speed   = SPI_SPEED_MODE_4MHz,
    .spi_wsz     = SPI_MODE_8BIT,
    .spi_cs      = SPI_CS_0,
    .cs_pad.port = FLASH_CS_PORT,
    .cs_pad.pin  = FLASH_CS_PIN,
};

static const spi_flash_cfg_t flash_cfg = {
    .chip_size = FLASH_CHIP_SIZE,
};

fw_mailbox_t fw_mb;

/* Progress markers at a fixed address well clear of our image (.bss ends
 * ~0x07FC6000) and of the stack (which grows down from ~0x07FD4800). NOT in
 * .bss on purpose: startup zeroes .bss, but SysRAM survives a SYSRESETREQ, so
 * these can be read back *after* resetting a core that has stopped responding
 * to the debugger - which is the only way to see how far it got. */
#define FW_TRACE  ((volatile uint32_t *)0x07FD0000)

static void flash_pins_init(void)
{
    GPIO_ConfigurePin(FLASH_CS_PORT,  FLASH_CS_PIN,  OUTPUT, PID_SPI_EN,  true);
    GPIO_ConfigurePin(FLASH_CLK_PORT, FLASH_CLK_PIN, OUTPUT, PID_SPI_CLK, false);
    GPIO_ConfigurePin(FLASH_DO_PORT,  FLASH_DO_PIN,  OUTPUT, PID_SPI_DO,  false);
    GPIO_ConfigurePin(FLASH_DI_PORT,  FLASH_DI_PIN,  INPUT,  PID_SPI_DI,  false);
}

/* Re-assert the watchdog freeze.
 *
 * Freezing once at init is not enough: the SDK's flash driver writes
 * WATCHDOG_REG from inside its own wait loops (spi_flash.c ~line 1398), so the
 * freeze can lapse across a flash operation. An expiring watchdog resets the
 * part, the boot ROM then runs against a flash we are midway through erasing,
 * and the debugger can no longer halt anything - which is exactly the failure
 * being chased. Cheap enough to call around every operation. */
static void wdog_freeze(void)
{
    SetWord16(SET_FREEZE_REG, FRZ_WDOG);
}

static uint32_t do_command(uint32_t cmd)
{
    uint32_t actual = 0;
    int8_t   rc     = SPI_FLASH_ERR_OK;

    wdog_freeze();

    switch (cmd) {
    case FW_CMD_INFO: {
        uint32_t id = 0;
        rc = spi_flash_read_jedec_id(&id);
        fw_mb.info = id;
        break;
    }
    case FW_CMD_ERASE:
        rc = spi_flash_chip_erase();
        break;

    case FW_CMD_SECTOR:
        rc = spi_flash_block_erase(fw_mb.addr, SPI_FLASH_OP_SE);
        break;

    case FW_CMD_WRITE: {
        /* Page-program in a loop rather than calling spi_flash_write_data():
         * that one wedges the part hard on this flash (a Fudan FM25Q-series,
         * JEDEC 0xA14013) - the core stops responding to the debugger
         * entirely and only a reset recovers it. spi_flash_page_program() on
         * the same data works, so the page splitting is done here instead. */
        uint32_t done = 0;
        if (fw_mb.len > FW_CHUNK) return FW_ST_ERR;
        FW_TRACE[0] = 0xA0000000u;      /* entered the write handler */
        while (done < fw_mb.len) {
            uint32_t at   = fw_mb.addr + done;
            uint32_t room = 256u - (at & 0xFFu);      /* to the page boundary */
            uint32_t n    = fw_mb.len - done;
            if (n > room) n = room;
            FW_TRACE[1] = 0xA1000000u | done;         /* about to program */
            rc = spi_flash_page_program((uint8_t *)fw_mb.data + done, at,
                                        (uint16_t)n);
            wdog_freeze();                            /* driver may have reloaded it */
            FW_TRACE[2] = 0xA2000000u | ((uint32_t)(uint8_t)rc);  /* returned */
            if (rc != SPI_FLASH_ERR_OK) break;
            done += n;
            FW_TRACE[3] = 0xA3000000u | done;         /* page committed */
        }
        FW_TRACE[4] = 0xA4000000u | done;             /* loop finished */
        actual = done;
        break;
    }

    case FW_CMD_STRESS:
        /* Continuous erase+program cycling, for putting a steady, repeating
         * load on the supply while someone watches a meter. Never returns -
         * reset to stop it. Counts completed cycles into FW_TRACE[6], which
         * survives the reset: a high count proves the core kept running the
         * whole time and only the debug interface went away. */
        for (uint32_t cycles = 0;; cycles++) {
            spi_flash_block_erase(0, SPI_FLASH_OP_SE);
            for (uint32_t p = 0; p < FW_CHUNK; p += 256) {
                spi_flash_page_program((uint8_t *)fw_mb.data + p, p, 256);
            }
            FW_TRACE[6] = 0xC0000000u | cycles;
        }

    case FW_CMD_PAGE:
        /* One page, lowest-level call available - used to tell a driver-level
         * problem in spi_flash_write_data()'s page splitting apart from the
         * page program itself failing. */
        if (fw_mb.len > 256) return FW_ST_ERR;
        rc = spi_flash_page_program((uint8_t *)fw_mb.data, fw_mb.addr,
                                    (uint16_t)fw_mb.len);
        break;

    case FW_CMD_READ:
        if (fw_mb.len > FW_CHUNK) return FW_ST_ERR;
        rc = spi_flash_read_data((uint8_t *)fw_mb.data, fw_mb.addr,
                                 fw_mb.len, &actual);
        if (rc == SPI_FLASH_ERR_OK && actual != fw_mb.len) rc = -1;
        break;

    default:
        return FW_ST_ERR;
    }

    if (rc != SPI_FLASH_ERR_OK) {
        fw_mb.info = (uint32_t)(int32_t)rc;
        return FW_ST_ERR;
    }
    return FW_ST_OK;
}

void flash_writer_main(void)
{
    uint8_t dev_id = 0;

    /* A chip erase takes seconds - far longer than the watchdog allows, and
     * there is no BLE stack running here to pet it. Freeze it outright.
     *
     * SET_FREEZE_REG freezes; RESET_FREEZE_REG (0x50003302) *un*freezes and is
     * what the SDK uses to force a reset. Writing that one here is a silent
     * time bomb: the stub comes up fine, answers for a couple of seconds, then
     * the watchdog reboots into the stock flash image mid-session. */
    SetWord16(SET_FREEZE_REG, FRZ_WDOG);

    /* Boot counter, self-initialising so a clobbered trace area re-seeds
     * itself rather than reporting nonsense. Any value above 1 after a single
     * RAM-load is proof the part reset behind our back - which is otherwise
     * invisible, since .bss is re-zeroed on every boot and the DA14585 has no
     * reset-source register to interrogate. */
    if (FW_TRACE[8] != 0x600DBEEFu) {
        FW_TRACE[8] = 0x600DBEEFu;
        FW_TRACE[9] = 0;
    }
    FW_TRACE[9] = FW_TRACE[9] + 1;

    fw_mb.cmd    = FW_CMD_NONE;
    fw_mb.status = FW_ST_BUSY;
    fw_mb.magic  = FW_MAGIC;

    flash_pins_init();
    spi_flash_configure_env(&flash_cfg);
    spi_initialize(&flash_spi_cfg);

    if (spi_flash_enable_with_autodetect(&flash_spi_cfg, &dev_id) != SPI_FLASH_ERR_OK) {
        /* Fall through anyway: autodetect failing on an unknown JEDEC id is
         * not fatal, the configured chip_size still drives the operations.
         * The host can tell from FW_CMD_INFO whether the part responds. */
    }
    /* Clear the block-protect bits. Parts ship with them set, and the stock
     * firmware may have set them too; with protection on, a page program is
     * accepted but does nothing while the driver waits for a completion that
     * never comes. The SDK's own spi_flash example does this before writing. */
    spi_flash_configure_memory_protection(SPI_FLASH_MEM_PROT_NONE);

    fw_mb.info   = dev_id;
    fw_mb.status = FW_ST_OK;    /* stub is live and the bus is up */

    for (;;) {
        uint32_t cmd = fw_mb.cmd;
        if (cmd == FW_CMD_NONE) {
            /* Back off before polling again. A bare `continue` spin hammers
             * the AHB hard enough to starve the debugger's own accesses: the
             * host then can't halt the core ("CPU could not be halted") and
             * long w4 bursts die partway through, which looks exactly like a
             * crashed or wedged target. It isn't - the core is fine, it just
             * never yields the bus. Verified with RAM trace markers that
             * survive a reset: the flash op completes and returns OK, and
             * only then does the core stop answering. */
            volatile uint32_t d = 0;
            while (d < 2000u) {
                d++;
            }
            wdog_freeze();
            continue;
        }
        fw_mb.status = FW_ST_BUSY;
        uint32_t st  = do_command(cmd);
        fw_mb.status = st;
        fw_mb.cmd    = FW_CMD_NONE;   /* cleared last - completion signal */
    }
}

#endif // EPD_FLASH_WRITER
