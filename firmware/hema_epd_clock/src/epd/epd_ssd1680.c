/**
 * epd_ssd1680.c
 *
 * Init sequence and LUT tables below now follow Waveshare's own
 * EPD_2IN13_V2 reference driver verbatim (STM32-F103ZET6/User/e-Paper/
 * EPD_2in13_V2.c, downloaded from the Baidu link the vendor's FAQ pointed
 * to - see PROTOCOL_NOTES.md section 2/11). That's a strictly better
 * starting point than this file's original all-zero LUT placeholders and
 * guessed reset timing: it's a verified-working reference for the exact
 * controller family this panel uses, explicitly cited by name in the
 * vendor's own docs.
 *
 * Cross-checked against the community DA14585 .bin's own epd_init: same
 * command set (0x01/0x0C/0x11/0x2C/0x32/0x3A/0x3B/0x3C/0x44/0x45/0x4E/0x4F/
 * 0x24/0x22/0x20), confirming the controller family identification, though
 * the community firmware's specific immediate values differ (e.g. a
 * shorter 30-byte LUT vs. Waveshare's 76-byte table, different VCOM/dummy-
 * line/gate-width tuning) - likely a simplified/re-tuned waveform specific
 * to whichever exact panel batch the community developer had. Since the
 * Waveshare sequence is verified-working for this same panel family, it's
 * used wholesale here rather than guessing at a mix of the two.
 *
 * BUSY polarity confirmed active-high-while-busy by Waveshare's own
 * EPD_2IN13_V2_ReadBusy() comment ("LOW: idle, HIGH: busy") - matches what
 * this file already assumed.
 */

#include "epd_ssd1680.h"
#include "spi.h"
#include "gpio.h"
#include "systick.h"    // systick_wait() blocking delay
#include "arch_wdg.h"   // wdg_reload() to survive the long refresh wait

/* Pixel polarity, CONFIRMED ON HARDWARE (2026-07-25).
 *
 * Our framebuffer uses 1 = white / 0 = black (matching the vendor's drawing
 * DSL and its canvas2bytes() image encoder). This panel's controller RAM uses
 * the SAME convention, so the bytes go out verbatim - no inversion.
 *
 * Waveshare's EPD_2IN13_V2_Display() sends ~Image only because its own GUI
 * layer stores 1 = black. Copying that inverted our first test pattern
 * (white shapes on a black field); setting this to 0 fixed it. */
#ifndef EPD_INVERT_OUTPUT
#define EPD_INVERT_OUTPUT 0
#endif

/* ---- low level cmd/data primitives -------------------------------------- */

/* CS is a plain GPIO (see epd_ssd1680.h) — active low. */
static void epd_cs_low(void)  { GPIO_SetInactive(EPD_CS_PORT, EPD_CS_PIN); }
static void epd_cs_high(void) { GPIO_SetActive(EPD_CS_PORT, EPD_CS_PIN); }

static void epd_write_cmd(uint8_t cmd)
{
    GPIO_SetInactive(EPD_DC_PORT, EPD_DC_PIN); /* DC low = command */
    epd_cs_low();
    spi_send(&cmd, 1, SPI_OP_BLOCKING);
    epd_cs_high();
}

static void epd_write_data(uint8_t data)
{
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);   /* DC high = data */
    epd_cs_low();
    spi_send(&data, 1, SPI_OP_BLOCKING);
    epd_cs_high();
}

static void epd_write_data_buf(const uint8_t *buf, uint16_t len)
{
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);
    epd_cs_low();
    spi_send(buf, len, SPI_OP_BLOCKING);
    epd_cs_high();
}

/* Accurate blocking delay via the SysTick timer. systick_wait() takes
 * microseconds; it caps around ~1s per call at 16MHz, and our longest use is
 * 200ms, so a single call is fine. */
static void epd_delay_ms(uint32_t ms)
{
    systick_wait(ms * 1000);
}

static void epd_wait_busy(void)
{
    /* BUSY is active-high while busy (1 = busy, 0 = idle), confirmed by the
     * Waveshare EPD_2IN13_V2 reference. A full refresh takes ~2s, so reload
     * the watchdog while we spin or the SoC will reset mid-refresh. The
     * timeout is a safety net in case BUSY never deasserts (e.g. wrong pin
     * or inverted polarity) — without it a bad build would hang here. */
    uint32_t timeout = 600000;   /* ~ a few seconds of spinning */
    while (GPIO_GetPinStatus(EPD_BUSY_PORT, EPD_BUSY_PIN) && timeout--) {
        wdg_reload(0xFF);        /* keep the watchdog happy during the wait */
    }
}

static void epd_hw_reset(void)
{
    /* Timing matches Waveshare's EPD_2IN13_V2_Reset() exactly. */
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(200);
    GPIO_SetInactive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(2);
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(200);
}

/* ---- LUT tables ------------------------------------------------------------
 * Verbatim from Waveshare's EPD_2IN13_V2_lut_full_update /
 * EPD_2IN13_V2_lut_partial_update. Layout: bytes[0:35) are the 5 waveform
 * groups (BB/BW/WB/WW/VCOM x 7 voltage-source steps), bytes[35:70) are the
 * 7 timing-period groups (A/B/C/D/repeat-count), and the trailing 6 bytes
 * [70:76) are NOT sent via cmd 0x32 - they're reused directly as the data
 * for cmd 0x03 (gate driving voltage), 0x04 (source driving voltage, 3
 * bytes), 0x3A (dummy line period) and 0x3B (gate line width), exactly as
 * epd_init() below does. */
static const uint8_t epd_lut_full[76] = {
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    0x03,0x03,0x00,0x00,0x02,
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

static const uint8_t epd_lut_partial[76] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    0x0A,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

/* ---- public API ----------------------------------------------------------- */

void epd_gpio_init(void)
{
    /* CS as GPIO output, idle high (inactive). Must be PID_GPIO so the
     * GPIO SET/RESET registers (used by epd_cs_low/high) actually drive it. */
    GPIO_ConfigurePin(EPD_CS_PORT,   EPD_CS_PIN,   OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(EPD_DC_PORT,   EPD_DC_PIN,   OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_RST_PORT,  EPD_RST_PIN,  OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(EPD_BUSY_PORT, EPD_BUSY_PIN, INPUT,  PID_GPIO, false);
    /* Assert the panel power-enable exactly as the stock firmware does
     * (P2_3 high). Without this the booster/supply may stay off. */
    GPIO_ConfigurePin(EPD_PWR_PORT,  EPD_PWR_PIN,  OUTPUT, PID_GPIO, true);
}

void epd_init(bool full_lut)
{
    const uint8_t *lut = full_lut ? epd_lut_full : epd_lut_partial;

    epd_hw_reset();

    if (full_lut) {
        epd_wait_busy();
        epd_write_cmd(0x12); /* SW Reset */
        epd_wait_busy();

        epd_write_cmd(0x74); /* Set analog block control */
        epd_write_data(0x54);
        epd_write_cmd(0x7E); /* Set digital block control */
        epd_write_data(0x3B);

        epd_write_cmd(0x01); /* Driver Output Control */
        epd_write_data((EPD_HEIGHT - 1) & 0xFF);
        epd_write_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
        epd_write_data(0x00);

        /* Data Entry Mode. Bits [1:0] = ID (Y dir, X dir), bit [2] = AM.
         *   0x01 = Y DECREMENT, X increment  <- Waveshare's value
         *   0x03 = Y increment, X increment  <- what we want
         * Waveshare's GUI layer feeds rows bottom-up, so Y-decrement suits it.
         * Our framebuffer is plain top-down (row 0 = y 0), so Y-decrement
         * mirrored the whole image top-to-bottom - confirmed on hardware, the
         * digits rendered upside-down (6<->9) while staying in left-to-right
         * order, i.e. Y flipped and X correct. */
        epd_write_cmd(0x11);
        epd_write_data(0x03);

        epd_write_cmd(0x44); /* Set RAM X address start/end */
        epd_write_data(0x00);
        epd_write_data((EPD_WIDTH_BYTES - 1) & 0xFF);

        /* RAM Y window: start at 0 and count UP to H-1, to match Y-increment. */
        epd_write_cmd(0x45);
        epd_write_data(0x00);
        epd_write_data(0x00);
        epd_write_data((EPD_HEIGHT - 1) & 0xFF);
        epd_write_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);

        epd_write_cmd(0x3C); /* Border Waveform Control */
        epd_write_data(0x03);

        epd_write_cmd(0x2C); /* Write VCOM Register */
        epd_write_data(0x55);

        epd_write_cmd(0x03); /* Gate driving voltage */
        epd_write_data(lut[70]);

        epd_write_cmd(0x04); /* Source driving voltage */
        epd_write_data(lut[71]);
        epd_write_data(lut[72]);
        epd_write_data(lut[73]);

        epd_write_cmd(0x3A); /* Dummy Line Period */
        epd_write_data(lut[74]);
        epd_write_cmd(0x3B); /* Gate Line Width */
        epd_write_data(lut[75]);

        epd_write_cmd(0x32); /* Write LUT Register - first 70 bytes only */
        epd_write_data_buf(lut, 70);

        /* RAM address counters to the window origin (0,0) - Y increments. */
        epd_write_cmd(0x4E);
        epd_write_data(0x00);
        epd_write_cmd(0x4F);
        epd_write_data(0x00);
        epd_write_data(0x00);
        epd_wait_busy();
    } else {
        epd_write_cmd(0x2C); /* Write VCOM Register (partial-mode value) */
        epd_write_data(0x26);
        epd_wait_busy();

        epd_write_cmd(0x32);
        epd_write_data_buf(lut, 70);

        epd_write_cmd(0x37); /* Set display option (partial mode) */
        epd_write_data(0x00);
        epd_write_data(0x00);
        epd_write_data(0x00);
        epd_write_data(0x00);
        epd_write_data(0x40);
        epd_write_data(0x00);
        epd_write_data(0x00);

        epd_write_cmd(0x22);
        epd_write_data(0xC0);
        epd_write_cmd(0x20);
        epd_wait_busy();

        epd_write_cmd(0x3C); /* Border Waveform Control */
        epd_write_data(0x01);
    }
}

void epd_display(const uint8_t *framebuffer)
{
    uint32_t i;

    /* Start at the window origin (0,0). Y counts UP - see the Data Entry Mode
     * note in epd_init(); starting at H-1 would mirror the image vertically. */
    epd_write_cmd(0x4E); /* Set RAM X address counter */
    epd_write_data(0x00);
    epd_write_cmd(0x4F); /* Set RAM Y address counter */
    epd_write_data(0x00);
    epd_write_data(0x00);

    /* Write RAM (B/W). Our framebuffer uses 1 = white (matching the vendor's
     * DSL where color 1 = white, and the vendor image encoder), but the panel
     * RAM is the opposite polarity — Waveshare's own EPD_2IN13_V2_Display
     * sends ~Image for exactly this reason. So invert on the way out. If the
     * very first test pattern comes out as white-on-black (inverted), flip
     * EPD_INVERT_OUTPUT to 0 and reflash. */
    epd_write_cmd(0x24);
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);   /* DC high = data */
    epd_cs_low();
    for (i = 0; i < EPD_BUF_SIZE; i++) {
        uint8_t b = framebuffer[i];
#if EPD_INVERT_OUTPUT
        b = (uint8_t)~b;
#endif
        spi_send(&b, 1, SPI_OP_BLOCKING);
    }
    epd_cs_high();

    epd_write_cmd(0x22); /* Display Update Control 2: full refresh sequence */
    epd_write_data(0xC7);
    epd_write_cmd(0x20); /* Master Activation */
    epd_wait_busy();
}

void epd_sleep(void)
{
    epd_write_cmd(0x22); /* POWER OFF */
    epd_write_data(0xC3);
    epd_write_cmd(0x20);

    epd_write_cmd(0x10); /* Deep Sleep Mode */
    epd_write_data(0x01);
    epd_delay_ms(100);
}
