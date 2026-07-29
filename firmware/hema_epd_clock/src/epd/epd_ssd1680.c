/**
 * epd_ssd1680.c
 *
 * There are two init sequences here, picked by EPD_INIT_FROM_OTP - which
 * follows the board variant, not the panel size. See the comment on that macro
 * in epd_ssd1680.h for why.
 *
 * VARIANT A takes the sequence from those tags' own retail firmware and loads
 * the waveform from the controller's OTP. One sequence covers both panel
 * sizes, because the retail firmware points both panel descriptors at it. See
 * epd_init() below and hema-local/re/type3/README.md.
 *
 * VARIANT B is the Waveshare-derived path described below, with a hand-written
 * LUT. It is the one proven on the Type 1 tag.
 *
 * Everything below this point describes the variant-B path only.
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

/* Stamp the board variant into the image so tools/flash.sh can check it
 * against the variant named on its command line before programming a tag.
 *
 * `used` is not optional: nothing in the firmware reads this, and the build
 * is -flto, so without it the string is dropped and the flasher would see
 * every image as variant-less. */
__attribute__((used))
const char epd_board_variant_tag[] = EPD_BOARD_VARIANT_TAG;

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

#if EPD_BITBANG

/* Variant A drives the panel with two plain GPIOs instead of the SPI block,
 * exactly as that board's retail firmware does. Mode 0: clock idles low, the
 * bit is presented while SCK is low and the panel latches it on the rising
 * edge; MSB first. Both were read off the retail driver's own bit loop rather
 * than assumed - it tests bit 7 first and pulses SCK high-then-low per bit.
 *
 * No delays: a GPIO_Set* pair is several 16 MHz cycles on its own, which is
 * far slower than the ~20 MHz this controller family accepts. */
static void epd_tx(const uint8_t *buf, uint16_t len)
{
    while (len--) {
        uint8_t b = *buf++;

        for (uint8_t i = 0; i < 8; i++) {
            if (b & 0x80) {
                GPIO_SetActive(EPD_SDA_PORT, EPD_SDA_PIN);
            } else {
                GPIO_SetInactive(EPD_SDA_PORT, EPD_SDA_PIN);
            }
            GPIO_SetActive(EPD_SCK_PORT, EPD_SCK_PIN);
            GPIO_SetInactive(EPD_SCK_PORT, EPD_SCK_PIN);
            b = (uint8_t)(b << 1);
        }
    }
}

#else

static void epd_tx(const uint8_t *buf, uint16_t len)
{
    spi_send(buf, len, SPI_OP_BLOCKING);
}

#endif

static void epd_write_cmd(uint8_t cmd)
{
    GPIO_SetInactive(EPD_DC_PORT, EPD_DC_PIN); /* DC low = command */
    epd_cs_low();
    epd_tx(&cmd, 1);
    epd_cs_high();
}

static void epd_write_data(uint8_t data)
{
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);   /* DC high = data */
    epd_cs_low();
    epd_tx(&data, 1);
    epd_cs_high();
}

static void epd_write_data_buf(const uint8_t *buf, uint16_t len)
{
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);
    epd_cs_low();
    epd_tx(buf, len);
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
#if EPD_INIT_FROM_OTP
    /* Variant A's own retail driver holds every phase for 100ms, including the
     * low pulse - where Waveshare's is 2ms. Transcribed from the reset routine
     * at 0x07FC3960 in a stock image. */
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(100);
    GPIO_SetInactive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(100);
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(100);
#else
    /* Timing matches Waveshare's EPD_2IN13_V2_Reset() exactly. */
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(200);
    GPIO_SetInactive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(2);
    GPIO_SetActive(EPD_RST_PORT, EPD_RST_PIN);
    epd_delay_ms(200);
#endif
}

#if !EPD_INIT_FROM_OTP

/* ---- LUT tables ------------------------------------------------------------
 * High-res (A53) panels only. The low-res A41 takes its waveform from the
 * controller's own OTP instead and never issues cmd 0x32 - see epd_init().
 *
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

#endif  /* !EPD_INIT_FROM_OTP */

/* ---- public API ----------------------------------------------------------- */

#if EPD_BITBANG

/* Variant A shares nothing with the boot flash: the panel is on P0_1/P2_0 and
 * the flash on P0_0/P0_3/P0_5/P0_6, so there is no bus to hand back. This stays
 * as a real function rather than an empty macro because epd_store.c calls it on
 * release and the panel's pads still have to be put back to outputs - the flash
 * path does not touch them, but a future borrower might. */
void epd_spi_claim(void)
{
    GPIO_ConfigurePin(EPD_SCK_PORT, EPD_SCK_PIN, OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_SDA_PORT, EPD_SDA_PIN, OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_DC_PORT,  EPD_DC_PIN,  OUTPUT, PID_GPIO, false);
}

#else

/* The panel does not own the SPI bus outright: the boot flash hangs off the
 * same CLK (P0_0) and MOSI (P0_6), and worse, P0_5 is the panel's D/C but the
 * flash's MISO. Whoever used the bus last must therefore hand it back before
 * the other can use it - see epd_store.c, which calls this on release. */
static const spi_cfg_t epd_spi_cfg = {
    .spi_ms    = SPI_MS_MODE_MASTER,
    .spi_cp    = SPI_CP_MODE_0,
    .spi_speed = SPI_SPEED_MODE_8MHz,
    .spi_wsz   = SPI_MODE_8BIT,
    .spi_cs    = SPI_CS_0,
    .spi_irq   = SPI_IRQ_DISABLED,
    /* Same pad as SPI_EN_PORT/PIN in user_periph_setup.h, named here in the
     * panel driver's own terms - CS is driven manually by epd_cs_low/high. */
    .cs_pad    = { .port = EPD_CS_PORT, .pin = EPD_CS_PIN },
};

void epd_spi_claim(void)
{
    /* D/C back to a plain output - the flash driver leaves it as SPI_DI. */
    GPIO_ConfigurePin(EPD_DC_PORT, EPD_DC_PIN, OUTPUT, PID_GPIO, false);
    spi_initialize(&epd_spi_cfg);
}

#endif  /* EPD_BITBANG */

void epd_gpio_init(void)
{
#if EPD_BITBANG
    /* Panel clock and data as plain outputs, both idle low. */
    GPIO_ConfigurePin(EPD_SCK_PORT, EPD_SCK_PIN, OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_SDA_PORT, EPD_SDA_PIN, OUTPUT, PID_GPIO, false);
    /* P2_2 is in the retail firmware's pin table, so we drive it as the retail
     * firmware does - but on this board it goes nowhere. Traced on the Type 3
     * PCB it lands on R22, an unpopulated resistor position, and stops. It was
     * described here as a "second enable line"; that was a guess and it was
     * wrong. Kept because driving an unconnected pad costs nothing and other
     * board revisions may well populate R22. */
    GPIO_ConfigurePin(EPD_AUX_PORT, EPD_AUX_PIN, OUTPUT, PID_GPIO, true);
#endif

    /* CS as GPIO output, idle high (inactive). Must be PID_GPIO so the
     * GPIO SET/RESET registers (used by epd_cs_low/high) actually drive it. */
    GPIO_ConfigurePin(EPD_CS_PORT,   EPD_CS_PIN,   OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(EPD_DC_PORT,   EPD_DC_PIN,   OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_RST_PORT,  EPD_RST_PIN,  OUTPUT, PID_GPIO, true);
    /* BUSY as an input with a PULL-DOWN, not hi-Z.
     *
     * The retail firmware configures its BUSY pin this way (mode 0x0200,
     * confirmed by reading the pin back after calling its own panel init), and
     * the reason is worth keeping: BUSY is active-high, so a hi-Z input on a
     * panel that is not driving the line floats high and reads as **busy
     * forever**. That turns "the panel is not connected" into "the panel is
     * permanently mid-refresh" - epd_display_busy() never returns false, the
     * refresh never completes, and every wait here burns its full timeout.
     * Diagnosing that cost most of a session on the variant-A tag.
     *
     * With a pull-down the same fault reads as idle, which is both the safer
     * failure and the honest one. On a healthy panel the line is driven either
     * way, so this costs nothing. */
    GPIO_ConfigurePin(EPD_BUSY_PORT, EPD_BUSY_PIN, INPUT_PULLDOWN, PID_GPIO, false);
    /* Assert the panel power-enable exactly as the stock firmware does
     * (P2_3 high). Without this the booster/supply may stay off. */
    GPIO_ConfigurePin(EPD_PWR_PORT,  EPD_PWR_PIN,  OUTPUT, PID_GPIO, true);
}

#if EPD_BITBANG && EPD_PANEL_PROBE

/* Is a panel actually answering?
 *
 * The retail firmware asks this before it draws anything - the routine at
 * 0x07FC3BD6 resets the panel, sends 0x2F (Read Status Bit) and clocks one
 * byte back, treating 0xFF as "no panel". Worth having for the same reason it
 * had it: every other symptom of a disconnected panel looks like a driver bug.
 * A dead BUSY line reads as permanently idle, so a refresh "succeeds" in no
 * time and leaves the screen untouched, which is indistinguishable from a bad
 * init sequence unless you ask the controller directly.
 *
 * The read is the retail one, clocked the same way: CS low, D/C high, SDA
 * turned around to an input, then 8 x {SCK low, sample, SCK high}, MSB first.
 *
 * Sampled twice, once against each internal pull, which is what makes the
 * answer trustworthy. A panel that is there drives the line and both passes
 * agree; a line nobody is driving simply follows the pull and they come back
 * 0xFF and 0x00. That distinguishes "absent" from "present and reporting all
 * ones", which a single read cannot.
 *
 * Results are left here for a debugger to read - see epd_probe_pullup /
 * epd_probe_pulldown. Nothing in the firmware acts on them: a tag whose panel
 * has come loose should still keep its clock and its BLE link. */
volatile uint8_t epd_probe_pullup;
volatile uint8_t epd_probe_pulldown;

static uint8_t epd_read_byte(uint32_t sda_mode)
{
    uint8_t v = 0;

    epd_cs_low();
    GPIO_SetInactive(EPD_SCK_PORT, EPD_SCK_PIN);
    GPIO_SetActive(EPD_DC_PORT, EPD_DC_PIN);      /* data phase */
    GPIO_ConfigurePin(EPD_SDA_PORT, EPD_SDA_PIN,
                      (GPIO_PUPD)sda_mode, PID_GPIO, false);

    for (uint8_t i = 0; i < 8; i++) {
        GPIO_SetInactive(EPD_SCK_PORT, EPD_SCK_PIN);
        v = (uint8_t)((v << 1) |
                      (GPIO_GetPinStatus(EPD_SDA_PORT, EPD_SDA_PIN) ? 1u : 0u));
        GPIO_SetActive(EPD_SCK_PORT, EPD_SCK_PIN);
    }

    GPIO_ConfigurePin(EPD_SDA_PORT, EPD_SDA_PIN, OUTPUT, PID_GPIO, false);
    epd_cs_high();
    return v;
}

bool epd_panel_present(void)
{
    epd_hw_reset();

    epd_write_cmd(0x2F);
    epd_probe_pullup = epd_read_byte(INPUT_PULLUP);

    epd_write_cmd(0x2F);
    epd_probe_pulldown = epd_read_byte(INPUT_PULLDOWN);

    return epd_probe_pullup == epd_probe_pulldown;
}

#endif  /* EPD_BITBANG && EPD_PANEL_PROBE */

void epd_init(bool full_lut)
{
#if EPD_INIT_FROM_OTP
    /* Transcribed from a variant-A tag's own retail firmware - the panel-init
     * routine at 0x07FC3960/0x07FC399E in re/type3/t3_bank1_running.bin.
     *
     * Geometry-independent on purpose. That firmware registers the same vtable
     * for its 104x212 and its 122x250 descriptors (both call the constructor at
     * 0x07FC3BF6), and the routine takes width and height as arguments, so this
     * one sequence is what the vendor uses to drive either panel. Everything
     * below reads its size from EPD_WIDTH/EPD_HEIGHT for the same reason.
     *
     * The decisive difference from the variant-B path below is that it writes
     * NO waveform. There is no cmd 0x32 anywhere on this path (nor cmd 0x2C /
     * 0x03 / 0x04 / 0x3A / 0x3B, which only exist to tune a hand-written one):
     * instead cmd 0x18 selects the internal temperature sensor and cmd 0x22
     * bit 4 tells the controller to load its own factory waveform from OTP.
     * Overriding that with a Waveshare LUT is what left the A41 going busy and
     * never coming back.
     *
     * Partial refresh has no equivalent here yet - nothing calls epd_init(false)
     * today, and working out the OTP path's partial mode needs a panel we can
     * actually refresh first. */
    (void)full_lut;

    epd_hw_reset();
    epd_write_cmd(0x12);    /* SW Reset */
    epd_wait_busy();

    /* Analog + digital block control. The retail driver sends 0x7E as a data
     * byte of 0x74 rather than as its own command, consistently in all four of
     * its panel drivers - so it is the vendor's idiom, not a transcription
     * slip on our side. Kept verbatim; this is the sequence that ships. */
    epd_write_cmd(0x74);
    epd_write_data(0x54);
    epd_write_data(0x7E);
    epd_write_data(0x3B);

    epd_write_cmd(0x2B);
    epd_write_data(0x04);
    epd_write_data(0x63);

    epd_write_cmd(0x0C);    /* Booster Soft Start Control */
    epd_write_data(0x8B);
    epd_write_data(0x9C);
    epd_write_data(0x96);
    epd_write_data(0x0F);

    epd_write_cmd(0x01);    /* Driver Output Control */
    epd_write_data((EPD_HEIGHT - 1) & 0xFF);
    epd_write_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    epd_write_data(0x00);

    /* Data Entry Mode and the RAM Y window deliberately keep OUR orientation
     * (Y increment, window 0..H-1) rather than the retail driver's Y-decrement.
     * Y direction is independent of the waveform problem, epd_display_start()
     * below already sets the Y counter to 0 to match, and this is the pairing
     * proven right way up on the high-res tag. If the first image that appears
     * is mirrored top-to-bottom, this is the line to revisit - but a mirrored
     * image would already mean the panel is refreshing. */
    epd_write_cmd(0x11);
    epd_write_data(0x03);

    epd_write_cmd(0x44);    /* RAM X window */
    epd_write_data(0x00);
    epd_write_data((EPD_WIDTH_BYTES - 1) & 0xFF);

    epd_write_cmd(0x45);    /* RAM Y window */
    epd_write_data(0x00);
    epd_write_data(0x00);
    epd_write_data((EPD_HEIGHT - 1) & 0xFF);
    epd_write_data(((EPD_HEIGHT - 1) >> 8) & 0xFF);

    epd_write_cmd(0x3C);    /* Border Waveform Control */
    epd_write_data(0x01);

    epd_write_cmd(0x18);    /* Temperature Sensor Control */
    epd_write_data(0x80);   /* use the controller's internal sensor */

    /* Load the temperature reading and the OTP waveform now, before any RAM
     * write. 0xB1 = enable clock, load temperature, load LUT - note it does
     * NOT display, unlike the 0xC7 epd_display_start() sends later. */
    epd_write_cmd(0x22);
    epd_write_data(0xB1);
    epd_write_cmd(0x20);    /* Master Activation */
    epd_wait_busy();
#else
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
#endif  /* EPD_INIT_FROM_OTP */
}

bool epd_display_busy(void)
{
    /* BUSY is active-high while busy, confirmed by the Waveshare reference. */
    return GPIO_GetPinStatus(EPD_BUSY_PORT, EPD_BUSY_PIN) ? true : false;
}

void epd_display_start(const uint8_t *framebuffer)
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
        epd_tx(&b, 1);
    }
    epd_cs_high();

    epd_write_cmd(0x22); /* Display Update Control 2: full refresh sequence */
    epd_write_data(0xC7);
    epd_write_cmd(0x20); /* Master Activation */

    /* Deliberately no epd_wait_busy() here. The panel drives BUSY high and
     * refreshes on its own for ~2 s; the caller polls epd_display_busy() from
     * a timer so the BLE stack keeps getting scheduled meanwhile. Everything
     * above is just SPI - about 4 ms for the 4000-byte RAM write at 8 MHz -
     * so returning now costs the link nothing. */
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
