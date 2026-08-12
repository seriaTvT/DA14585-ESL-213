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
#include "epd_board.h"  // epd_board_signal_t - the order s_pin[] is indexed by
#include "spi.h"
#include "gpio.h"
#include "systick.h"    // systick_wait() blocking delay
#include "arch_wdg.h"   // wdg_reload() to survive the long refresh wait
#if EPD_TEMP_SWEEP || EPD_PARTIAL
// epd_framebuffer[], the surface the sweep repaints; and epd_gfx_dirty_rows(),
// which decides how much of the panel a partial refresh has to touch.
#include "epd_gfx.h"
#endif

/* Stamp the board variant into the image so tools/flash.sh can check it
 * against the variant named on its command line before programming a tag.
 *
 * `used` is not optional: nothing in the firmware reads this, and the build
 * is -flto, so without it the string is dropped and the flasher would see
 * every image as variant-less. */
__attribute__((used))
const char epd_board_variant_tag[] = EPD_BOARD_VARIANT_TAG;

/* The panel geometry and the tag type, stamped the same way and for the same
 * reason - see EPD_PANEL_TAG. `used` for the same reason too: nothing reads
 * these, and -flto would otherwise drop them. */
__attribute__((used))
const char epd_panel_tag[] = EPD_PANEL_TAG;

__attribute__((used))
const char hema_tag_type_tag[] = HEMA_TAG_TYPE_TAG;

__attribute__((used))
const char hema_waveform_tag[] = HEMA_WAVEFORM_TAG;

/* Whether this image can itself be updated over the air. Present only when it
 * can, so its absence is the signal.
 *
 * Not part of HEMA_COMPAT_STR, because SUOTA is not a panel-compatibility axis -
 * an image without it drives the panel exactly as well. But pushing one over the
 * air is a one-way door: the tag that receives it has no SUOTA service
 * afterwards and needs SWD for every update from then on, which on a fleet means
 * opening the case. A client can see this stamp and say so before it happens. */
#if defined(EPD_SUOTA) && (EPD_SUOTA)
__attribute__((used))
const char hema_suota_tag[] = "HEMA-SUOTA-1";
#endif

/* And the sixteen-byte compatibility identity, for the over-the-air path.
 * tools/mksuota.py greps this one and copies what follows the prefix into the
 * SUOTA image header's version field; the same string without the prefix is
 * what the tag reports over BLE, so a client can compare the two before it
 * spends four minutes transferring an image the panel cannot use. */
__attribute__((used))
const char hema_compat_tag[] = HEMA_COMPAT_TAG;

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

/* ---- the pin map, at runtime -------------------------------------------- */

/* Which pin each panel signal is on, held in RAM rather than compiled in.
 *
 * The macros in epd_ssd1680.h still decide what goes in here, so this is not
 * yet a board-driven map - it is the same eight pins by a slower route, and
 * every build behaves exactly as it did. What it buys is that the driver no
 * longer NAMES its pins at each use, which is the thing that has to stop before
 * the map can come off the tag (epd/epd_board.h). That change is then the
 * initialiser below and nothing else.
 *
 * Indexed by epd_board_signal_t so the two describe the same eight signals in
 * the same order, and a record decoded from flash can be dropped straight in.
 *
 * WHAT THIS COSTS TODAY: nothing, and not for a flattering reason. Because
 * epd_pins_init() writes only compile-time constants, -flto propagates them
 * through every read and folds the table out of existence - on a Type 4 build
 * s_sda_set is a uint32_t that llvm-nm reports as one byte and that reads back
 * as 1 on the tag, because nothing loads it any more.
 *
 * So this arrangement is NOT yet evidence that a runtime map works. It cannot
 * be: the compiler removes the runtime part. What it is worth is that the 41
 * call sites no longer name their pins, which makes seeding the table from
 * flash a change to one function instead of a rewrite. The real cost - and a
 * real measurement - arrives with that change, when the values stop being
 * knowable at compile time. Expect both the size and the frame time to move
 * then, and do not treat today's numbers as the baseline. */
typedef struct {
    uint8_t port;
    uint8_t pin;
} epd_pin_t;

static epd_pin_t s_pin[EPD_BOARD_NPINS];

/* Both halves of a signal's identity, as the two arguments every GPIO_* call
 * wants. Spelled as one macro so a call site reads about as well as it did
 * when these were constants. */
#define EPD_PIN(sig) \
    (GPIO_PORT)s_pin[EPD_BOARD_##sig].port, (GPIO_PIN)s_pin[EPD_BOARD_##sig].pin

/* Seed the table from the compile-time map. Deliberately the only place that
 * mentions the EPD_*_PORT/PIN macros for these eight signals, so that pointing
 * the driver at a board record later is a change to one function.
 *
 * AUX is the exception and stays conditional: only variant A names one, and
 * only variant A drives it. The vendor's variant-B table does name a pin there
 * (P1_1) and drives it, but this firmware never has, and quietly starting to
 * during a refactor would be a behaviour change smuggled in under "no
 * behaviour change". It is a question for when the map comes off the tag. */
static void epd_pins_init(void)
{
    s_pin[EPD_BOARD_CS]   = (epd_pin_t){ EPD_CS_PORT,   EPD_CS_PIN   };
    s_pin[EPD_BOARD_RST]  = (epd_pin_t){ EPD_RST_PORT,  EPD_RST_PIN  };
    s_pin[EPD_BOARD_SCK]  = (epd_pin_t){ EPD_SCK_PORT,  EPD_SCK_PIN  };
    s_pin[EPD_BOARD_SDA]  = (epd_pin_t){ EPD_SDA_PORT,  EPD_SDA_PIN  };
    s_pin[EPD_BOARD_DC]   = (epd_pin_t){ EPD_DC_PORT,   EPD_DC_PIN   };
    s_pin[EPD_BOARD_BUSY] = (epd_pin_t){ EPD_BUSY_PORT, EPD_BUSY_PIN };
    s_pin[EPD_BOARD_PWR]  = (epd_pin_t){ EPD_PWR_PORT,  EPD_PWR_PIN  };
#ifdef EPD_AUX_PORT
    s_pin[EPD_BOARD_AUX]  = (epd_pin_t){ EPD_AUX_PORT,  EPD_AUX_PIN  };
#endif
}

#if EPD_BITBANG && EPD_TX_FAST
/* The four GPIO registers the bit loop writes, and the two bit masks, worked
 * out once instead of eight times per byte.
 *
 * The point of caching is that the loop never indexes s_pin, so a map that
 * varies per board costs the transfer one setup call per frame rather than two
 * lookups per bit. Recomputed by epd_spi_claim(), which already runs at init
 * and after every time the boot flash borrows the bus.
 *
 * Today -flto folds all six into immediates, because their inputs are
 * compile-time constants - see the note on s_pin. That stops when the map is
 * read from flash, and these become genuine loads. */
/* The four registers the loop writes, each as a whole address, plus the two bit
 * masks. Six values, and the exact six the loop wants in registers.
 *
 * Storing the two port BLOCKS instead and reaching set/reset as base+2 / base+4
 * looks tighter and is worse: it needs the base and both offsets live where an
 * address needs only itself, and Cortex-M0's eight low registers do not stretch
 * to it. Measured, not guessed - that form made the compiler spill the constant
 * 2 to the stack and reload it on every bit. */
static uint32_t s_sda_set, s_sda_clr, s_sck_set, s_sck_clr;
static uint16_t s_sda_bit, s_sck_bit;

/* Port 3's registers do not follow the others: P30_MODE_REG is at 0x50003086,
 * so the block sits at index 4 rather than 3. GPIO_SetActive() handles this and
 * plain `port << 5` does not.
 *
 * This used to be a compile-time refusal, on the grounds that no panel pin is
 * on port 3 in either variant. That was true of a map chosen by the compiler
 * and cannot be asserted about one read off a tag, so it is arithmetic now -
 * a conditional at setup time, nowhere near the loop. */
static uint32_t epd_gpio_block(uint8_t port)
{
    return GPIO_BASE + ((uint32_t)(port == 3u ? 4u : port) << 5);
}

/* Offsets within a port block. Named because the loop below reads better for
 * it, and because +2/+4 in isolation look like they could be either way round. */
#define GPIO_SET_OFF  2u
#define GPIO_CLR_OFF  4u

static void epd_tx_cache_pins(void)
{
    const uint32_t sda = epd_gpio_block(s_pin[EPD_BOARD_SDA].port);
    const uint32_t sck = epd_gpio_block(s_pin[EPD_BOARD_SCK].port);

    s_sda_set = sda + GPIO_SET_OFF;
    s_sda_clr = sda + GPIO_CLR_OFF;
    s_sck_set = sck + GPIO_SET_OFF;
    s_sck_clr = sck + GPIO_CLR_OFF;
    s_sda_bit = (uint16_t)(1u << s_pin[EPD_BOARD_SDA].pin);
    s_sck_bit = (uint16_t)(1u << s_pin[EPD_BOARD_SCK].pin);
}
#endif

/* ---- low level cmd/data primitives -------------------------------------- */

#if EPD_TX_PROFILE
/* How long the last full frame took to shift out, in microseconds, and how
 * many bytes that was. Diagnostic only, and off unless --tx-profile asked for
 * it: this is here to answer "how much of a refresh is the bus?" with a
 * measurement rather than an instruction count.
 *
 *     hema-local/tools/tagread.py <image.elf> epd_tx_us epd_tx_bytes
 *
 * Deliberately measures the RAM write, not a whole refresh: the panel's
 * waveform dominates the latter and would bury the difference this is for. */
volatile uint32_t epd_tx_us;
volatile uint32_t epd_tx_bytes;
static   uint32_t epd_tx_t0;
#endif

/* CS is a plain GPIO (see epd_ssd1680.h) — active low. */
static void epd_cs_low(void)  { GPIO_SetInactive(EPD_PIN(CS)); }
static void epd_cs_high(void) { GPIO_SetActive(EPD_PIN(CS)); }

#if EPD_BITBANG

/* Variant A drives the panel with two plain GPIOs instead of the SPI block,
 * exactly as that board's retail firmware does. Mode 0: clock idles low, the
 * bit is presented while SCK is low and the panel latches it on the rising
 * edge; MSB first. Both were read off the retail driver's own bit loop rather
 * than assumed - it tests bit 7 first and pulses SCK high-then-low per bit.
 *
 * No delays: a GPIO_Set* pair is several 16 MHz cycles on its own, which is
 * far slower than the ~20 MHz this controller family accepts. */

#if EPD_TX_FAST

static void epd_tx(const uint8_t *buf, uint16_t len)
{
    /* Write the GPIO set/reset registers directly rather than going through
     * GPIO_SetActive()/GPIO_SetInactive().
     *
     * Those are correct and stay in use everywhere else in this file. But
     * under DEVELOPMENT_DEBUG each one first tests a 64-bit pin-reservation
     * mask and __BKPT()s if the pin was never reserved - and this loop calls
     * them three times per bit, so the check runs 24 times per byte purely to
     * re-answer a question whose answer cannot change mid-frame. Counted off
     * the compiled loop it was about eighteen of the forty-one cycles a bit
     * cost. The allocation monitor keeps working for every other GPIO call;
     * this is the one place where it does not pay for itself.
     *
     * The addresses are exactly the arithmetic GPIO_SetActive() does: the port
     * block at base + (port << 5), +2 to set a bit, +4 to clear it. They are
     * worked out by epd_tx_cache_pins() rather than here, which is what keeps
     * the pin map free: the loop reads six cached values and never touches
     * s_pin[], so it costs the same whether the map was compiled in or read off
     * the tag. Copied into locals so the compiler keeps them in registers
     * across the whole frame instead of reloading each byte.
     *
     * Still no delays. Each store is a couple of 16 MHz cycles, so SCK's high
     * phase is ~125 ns - quicker than before, and still far inside what this
     * controller family accepts (~20 MHz). If a panel ever disagrees, this is
     * the first place to look, and the fix is a nop between the edges rather
     * than going back through the driver. */
    const uint32_t sda_set = s_sda_set, sda_clr = s_sda_clr;
    const uint32_t sck_set = s_sck_set, sck_clr = s_sck_clr;
    const uint16_t sda_bit = s_sda_bit, sck_bit = s_sck_bit;

    /* One bit, MSB first: present it, then pulse the clock. */
#define EPD_TX_BIT()                                                         \
    do {                                                                     \
        SetWord16((b & 0x80u) ? sda_set : sda_clr, sda_bit);                 \
        SetWord16(sck_set, sck_bit);                                         \
        SetWord16(sck_clr, sck_bit);                                         \
        b = (uint8_t)(b << 1);                                               \
    } while (0)

    /* Unrolled, and for a specific reason rather than out of habit.
     *
     * A bit counter is a ninth live value, and Cortex-M0's inner loop has
     * eight low registers. With it the compiler spilled the SDA base to the
     * stack and reloaded it ONCE PER BIT - a load in the middle of the tightest
     * loop in the firmware. Unrolling deletes the counter, its compare and its
     * branch, and the spill goes with them. */
    while (len--) {
        uint8_t b = *buf++;

        EPD_TX_BIT(); EPD_TX_BIT(); EPD_TX_BIT(); EPD_TX_BIT();
        EPD_TX_BIT(); EPD_TX_BIT(); EPD_TX_BIT(); EPD_TX_BIT();
    }
#undef EPD_TX_BIT
}

#else   /* EPD_TX_FAST - the original, kept so the two can be measured against
         * each other and as the fallback if a panel ever objects to the
         * faster edges. tools/build.sh --tx-slow. */

static void epd_tx(const uint8_t *buf, uint16_t len)
{
    while (len--) {
        uint8_t b = *buf++;

        for (uint8_t i = 0; i < 8; i++) {
            if (b & 0x80) {
                GPIO_SetActive(EPD_PIN(SDA));
            } else {
                GPIO_SetInactive(EPD_PIN(SDA));
            }
            GPIO_SetActive(EPD_PIN(SCK));
            GPIO_SetInactive(EPD_PIN(SCK));
            b = (uint8_t)(b << 1);
        }
    }
}

#endif  /* EPD_TX_FAST */

#else   /* EPD_BITBANG */

static void epd_tx(const uint8_t *buf, uint16_t len)
{
    spi_send(buf, len, SPI_OP_BLOCKING);
}

#endif

static void epd_write_cmd(uint8_t cmd)
{
    GPIO_SetInactive(EPD_PIN(DC)); /* DC low = command */
    epd_cs_low();
    epd_tx(&cmd, 1);
    epd_cs_high();
}

static void epd_write_data(uint8_t data)
{
    GPIO_SetActive(EPD_PIN(DC));   /* DC high = data */
    epd_cs_low();
    epd_tx(&data, 1);
    epd_cs_high();
}

static void epd_write_data_buf(const uint8_t *buf, uint16_t len)
{
    GPIO_SetActive(EPD_PIN(DC));
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
    while (GPIO_GetPinStatus(EPD_PIN(BUSY)) && timeout--) {
        wdg_reload(0xFF);        /* keep the watchdog happy during the wait */
    }
}

static void epd_hw_reset(void)
{
#if EPD_INIT_FROM_OTP
    /* Variant A's own retail driver holds every phase for 100ms, including the
     * low pulse - where Waveshare's is 2ms. Transcribed from the reset routine
     * at 0x07FC3960 in a stock image. */
    GPIO_SetActive(EPD_PIN(RST));
    epd_delay_ms(100);
    GPIO_SetInactive(EPD_PIN(RST));
    epd_delay_ms(100);
    GPIO_SetActive(EPD_PIN(RST));
    epd_delay_ms(100);
#else
    /* Timing matches Waveshare's EPD_2IN13_V2_Reset() exactly. */
    GPIO_SetActive(EPD_PIN(RST));
    epd_delay_ms(200);
    GPIO_SetInactive(EPD_PIN(RST));
    epd_delay_ms(2);
    GPIO_SetActive(EPD_PIN(RST));
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
#if EPD_LUT_STEPS == 7

static const uint8_t epd_lut_full[EPD_LUT_BYTES + EPD_LUT_TRAILER] = {
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

static const uint8_t epd_lut_partial[EPD_LUT_BYTES + EPD_LUT_TRAILER] = {
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

#else  /* EPD_LUT_STEPS == 10 - the shape measured on the A41 controller */

/* Waveshare's waveform transposed into ten steps. Every value above is kept and
 * nothing is invented: the same three active phases, the same voltage bytes, the
 * same repeat counts, the same trailing registers. The seven-step groups simply
 * become ten-step groups with the extra steps idle, which is what the extra
 * duration bytes read as anyway.
 *
 * Group order is Waveshare's, i.e. **group-major**: all ten steps of LUT0, then
 * all ten of LUT1, and so on. The probe cannot see this - voltage bytes do not
 * change the duration - so it is a choice, and it is the one that follows the
 * reference these tables come from. If the panel drives but the image is wrong in
 * a way that looks like the phases are shuffled, step-major is the other
 * arrangement to try, and it is a rewrite of these two tables and nothing else.
 *
 * Predicted duration, so this can be checked before it is looked at:
 * (3+3)x2 + (9+9)x2 + (3+3)x2 = 60 frames, at ~19.8 ms over ~230 ms of overhead,
 * is ~1420 ms - about 28 of the app's 50 ms polls. A table of the wrong shape runs
 * zero frames and measures ~230 ms, or 4 polls. */
static const uint8_t epd_lut_full[EPD_LUT_BYTES + EPD_LUT_TRAILER] = {
    /* voltages: five groups of ten.
     *
     * Step 3 is ours, not Waveshare's, and it repeats step 2's column exactly -
     * black settling in sub-phase A, white in B. Only the durations below are
     * weighted. Driving black there with white left at zero is what made black
     * bleed into its neighbours; see EPD_LUT_BLACK_FRAMES. */
    0x80,0x60,0x40,EPD_LUT_BLACK_LEVEL,
                        0x00,0x00,0x00,0x00,0x00,0x00,   /* LUT0  black->black */
    0x10,0x60,0x20,EPD_LUT_WHITE_LEVEL,
                        0x00,0x00,0x00,0x00,0x00,0x00,   /* LUT1  black->white */
    0x80,0x60,0x40,EPD_LUT_BLACK_LEVEL,
                        0x00,0x00,0x00,0x00,0x00,0x00,   /* LUT2  white->black */
    0x10,0x60,0x20,EPD_LUT_WHITE_LEVEL,
                        0x00,0x00,0x00,0x00,0x00,0x00,   /* LUT3  white->white */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,   /* LUT4  VCOM         */

    /* timing: ten groups of TPA TPB TPC TPD RP */
    0x03,0x03,0x00,0x00,0x02,
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    /* the settling step: A settles black, B holds white against the fringe */
    EPD_LUT_BLACK_FRAMES,EPD_LUT_WHITE_FRAMES,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

/* The partial pair, same transposition. Only the two transition groups drive, so
 * a pixel that is not changing is left alone - that is what makes it partial.
 *
 * One phase of 10 frames, and the repeat count is 0. That is not a mistake and it
 * is not zero passes: the probe put a marker of 16 in a duration byte with the
 * repeat count at zero and measured 317 ms of extra time, i.e. 16 frames. So a
 * repeat of 0 runs the phase once. Predicted: 230 + 10 x 19.8 = ~430 ms, about
 * 9 polls. */
static const uint8_t epd_lut_partial[EPD_LUT_BYTES + EPD_LUT_TRAILER] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    0x0A,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,

    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

#endif  /* EPD_LUT_STEPS */

#endif  /* !EPD_INIT_FROM_OTP */

/* ---- public API ----------------------------------------------------------- */

#if EPD_BITBANG

/* Take the panel's pads back as plain outputs, with the clock idle low.
 *
 * Called at init and, more importantly, by epd_store.c every time it finishes
 * with the boot flash. What that means depends on the board:
 *
 *   variant A - nothing was taken away. The panel is on P0_1/P2_0 and the flash
 *     on P0_0/P0_3/P0_5/P0_6, disjoint, so this is a no-op that stays a real
 *     function because the release path calls it and a future borrower might
 *     actually touch these.
 *
 *   variant B - the pads ARE the flash's. flash_bus_acquire() switched them to
 *     PID_SPI_CLK/PID_SPI_DO to talk to the flash, and this switches them back.
 *     Miss this and the panel's next transfer clocks nothing: the pins are
 *     still wired to the SPI block, so the GPIO set/reset registers write into
 *     a pad that is not listening to them, and the failure is silent - no
 *     error, just a screen that stops changing.
 *
 * D/C is restored on both, because on variant B the flash claims P0_5 as its
 * MISO and on variant A it is simply cheap to be consistent. */
void epd_spi_claim(void)
{
    GPIO_ConfigurePin(EPD_PIN(SCK), OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_PIN(SDA), OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_PIN(DC),  OUTPUT, PID_GPIO, false);
#if EPD_TX_FAST
    /* Re-derive the bit loop's register addresses here rather than once at
     * init. They cannot change today - the map is compiled in - but this is
     * the function that says "the panel has its pins back", which is exactly
     * the condition those cached addresses describe. Putting them anywhere
     * else would mean a future runtime remap had two places to remember. */
    epd_tx_cache_pins();
#endif
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
    GPIO_ConfigurePin(EPD_PIN(DC), OUTPUT, PID_GPIO, false);
    spi_initialize(&epd_spi_cfg);
}

#endif  /* EPD_BITBANG */

void epd_gpio_init(void)
{
    /* Decide which pins these are before configuring any of them. First thing
     * in the first function that touches the panel, so nothing can read s_pin[]
     * before it is filled - the whole driver goes through it now, and a zeroed
     * table would quietly mean "every signal is P0_0". */
    epd_pins_init();

#if EPD_BITBANG
    /* Panel clock and data as plain outputs, both idle low. */
    GPIO_ConfigurePin(EPD_PIN(SCK), OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_PIN(SDA), OUTPUT, PID_GPIO, false);
    /* P2_2 is in the retail firmware's pin table, so we drive it as the retail
     * firmware does - but on this board it goes nowhere. Traced on the Type 3
     * PCB it lands on R22, an unpopulated resistor position, and stops. It was
     * described here as a "second enable line"; that was a guess and it was
     * wrong. Kept because driving an unconnected pad costs nothing and other
     * board revisions may well populate R22.
     *
     * Variant A only. Variant B's retail firmware drives its P2_2 LOW rather
     * than high, and we have never established what it is for there, so this
     * firmware leaves it alone on that board - guarded on the pin being named
     * rather than on the variant, since "we have a pin for this" is the actual
     * precondition. */
#ifdef EPD_AUX_PORT
    GPIO_ConfigurePin(EPD_PIN(AUX), OUTPUT, PID_GPIO, true);
#endif
#endif

    /* CS as GPIO output, idle high (inactive). Must be PID_GPIO so the
     * GPIO SET/RESET registers (used by epd_cs_low/high) actually drive it. */
    GPIO_ConfigurePin(EPD_PIN(CS),   OUTPUT, PID_GPIO, true);
    GPIO_ConfigurePin(EPD_PIN(DC),   OUTPUT, PID_GPIO, false);
    GPIO_ConfigurePin(EPD_PIN(RST),  OUTPUT, PID_GPIO, true);
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
    GPIO_ConfigurePin(EPD_PIN(BUSY), INPUT_PULLDOWN, PID_GPIO, false);
    /* Assert the panel power-enable exactly as the stock firmware does
     * (P2_3 high). Without this the booster/supply may stay off. */
    GPIO_ConfigurePin(EPD_PIN(PWR),  OUTPUT, PID_GPIO, true);
}

#if (EPD_BITBANG && EPD_PANEL_PROBE) || EPD_TEMP_READ || EPD_PANEL_ID

/* Clock bytes back out of the controller, MSB first.
 *
 * The panel's data line is bidirectional - the controller drives it during a
 * read - so the pad has to be turned around to an input and clocked by hand.
 * Sampled while SCK is low and advanced on the rising edge, mirroring what
 * epd_tx() does on the way out. Both directions were read off the retail
 * driver's own bit loops rather than assumed; its read primitive is at
 * 0x07FC266C in the Type 3 image, and it turns the same pad around the same
 * way through the function pointer at +0x20 of its pin table.
 *
 * `sda_mode` is the internal pull to hold while reading. A real read wants
 * INPUT; epd_panel_present() deliberately reads twice with PULLUP and then
 * PULLDOWN, which is what distinguishes "the panel drove the line" from
 * "nobody did and it followed the pull".
 *
 * Split into turnaround / clock / restore so a multi-byte register can be read
 * with CS held low for the whole transaction. epd_read_byte() below is exactly
 * the sequence this file has always used; nothing about a single-byte read has
 * changed.
 */
static void epd_read_begin(uint32_t sda_mode)
{
#if !EPD_BITBANG
    /* Variant B writes through the hardware SPI block, so its clock and data
     * pads are PID_SPI_CLK/PID_SPI_DO and cannot be driven by hand as they
     * stand. Borrow them as GPIOs for the turnaround and give them back at
     * the end. CS is manual on both variants and D/C is already an output, so
     * those need nothing. */
    GPIO_ConfigurePin(EPD_PIN(SCK), OUTPUT, PID_GPIO, false);
#endif

    epd_cs_low();
    GPIO_SetInactive(EPD_PIN(SCK));
    GPIO_SetActive(EPD_PIN(DC));      /* data phase */
    GPIO_ConfigurePin(EPD_PIN(SDA),
                      (GPIO_PUPD)sda_mode, PID_GPIO, false);
}

static uint8_t epd_read_bits(void)
{
    uint8_t v = 0;

    for (uint8_t i = 0; i < 8; i++) {
        GPIO_SetInactive(EPD_PIN(SCK));
        v = (uint8_t)((v << 1) |
                      (GPIO_GetPinStatus(EPD_PIN(SDA)) ? 1u : 0u));
        GPIO_SetActive(EPD_PIN(SCK));
    }
    return v;
}

static void epd_read_end(void)
{
#if EPD_BITBANG
    GPIO_ConfigurePin(EPD_PIN(SDA), OUTPUT, PID_GPIO, false);
    epd_cs_high();
#else
    epd_cs_high();
    /* Back to the SPI block, exactly as set_pad_functions() configures them.
     * Note this does NOT re-run spi_initialize(): the block itself was never
     * touched, only which pads it reaches. */
    GPIO_ConfigurePin(EPD_PIN(SCK), OUTPUT, PID_SPI_CLK, false);
    GPIO_ConfigurePin(EPD_PIN(SDA), OUTPUT, PID_SPI_DO,  false);
#endif
}

#endif  /* read primitive */

#if (EPD_BITBANG && EPD_PANEL_PROBE) || EPD_TEMP_READ

static uint8_t epd_read_byte(uint32_t sda_mode)
{
    uint8_t v;

    epd_read_begin(sda_mode);
    v = epd_read_bits();
    epd_read_end();
    return v;
}

#endif  /* single-byte read */

#if EPD_TEMP_READ

volatile int8_t epd_temp_c = EPD_TEMP_UNREAD;

/* Ask the controller what temperature it thinks it is.
 *
 * This matters because of what the vendor does with the same number. Its
 * driver (Type 3 image, the panel init at 0x07FC2FAC) sends exactly what our
 * OTP path sends - 0x18/0x80 to select the internal sensor, then 0x22/0xB1 to
 * load the temperature and the OTP waveform - and then reads the register
 * back with 0x1B. It takes ONE byte, stores it, forces it to 0 if bit 7 is
 * set, and compares it against 10 as a signed value. Only if it is BELOW 10
 * does it write anything extra (0x3D, 0x3E, 0x3F).
 *
 * Three things follow, and they are the reason this function exists:
 *   - the register is whole degrees Celsius, signed, in the first byte. Not
 *     sixteenths, and the second byte is not needed;
 *   - between about 10 C and the top of the range the vendor's own driver
 *     does not vary at all, so a refresh that does not change when the tag is
 *     warmed proves nothing about the sensor;
 *   - a wrong reading is invisible from the outside, which is exactly the
 *     class of fault worth being able to measure directly.
 *
 * We deliberately do NOT clamp negatives to zero the way the vendor does.
 * That clamp exists to make its own `< 10` comparison safe; here the honest
 * value is the useful one, and a panel below freezing is a real state.
 */
int8_t epd_read_temperature(void)
{
    /* No 0x18/0x22/0x20 here: the caller has just done it. Repeating the load
     * on the Waveshare path would pull the OTP waveform back over the LUT
     * that path writes by hand - see the header. */
    epd_write_cmd(0x1B);        /* Read Temperature Register */
    epd_temp_c = (int8_t)epd_read_byte(INPUT);
    return epd_temp_c;
}

#endif  /* EPD_TEMP_READ */

#if !EPD_INIT_FROM_OTP
/* Install the hand-written waveform, and the five registers that only exist to
 * go with one. Factored out of epd_init() because a temperature load overwrites
 * all of it - see epd_resample_temperature(). */
static void epd_load_waveshare_lut(const uint8_t *lut)
{
    const uint8_t *tbl = lut;

#if EPD_LUT_GAIN != 1
    /* Scale the repeat count of each timing group - see EPD_LUT_GAIN in the
     * header for what this is trying to distinguish and why.
     *
     * Bounds come from the shape rather than being written in, so this stays
     * right at either EPD_LUT_STEPS. Voltages and the A/B/C/D durations are left
     * exactly alone, so the waveform's shape is unchanged and only the number of
     * frames it runs for grows. */
    static uint8_t scaled[EPD_LUT_BYTES];
    uint16_t i;

    for (i = 0u; i < EPD_LUT_BYTES; i++) {
        scaled[i] = lut[i];
    }
    for (i = EPD_LUT_TIMING; i < EPD_LUT_BYTES; i += EPD_LUT_TIMING_GROUP) {
        /* A group with no repeats is an unused phase. Scaling it would invent
         * drive where the table deliberately asks for none. */
        if (lut[i + 4u] != 0u) {
            uint32_t rep = (uint32_t)lut[i + 4u] * (uint32_t)EPD_LUT_GAIN;
            scaled[i + 4u] = (rep > 255u) ? 255u : (uint8_t)rep;
        }
    }
    tbl = scaled;
#endif

    epd_write_cmd(0x2C); /* Write VCOM Register */
    epd_write_data(0x55);

    /* The trailer, indexed off the payload length rather than written in, so the
     * five registers keep following the table whatever shape it is. */
    epd_write_cmd(0x03); /* Gate driving voltage */
    epd_write_data(lut[EPD_LUT_BYTES + 0u]);

    epd_write_cmd(0x04); /* Source driving voltage */
    epd_write_data(lut[EPD_LUT_BYTES + 1u]);
    epd_write_data(lut[EPD_LUT_BYTES + 2u]);
    epd_write_data(lut[EPD_LUT_BYTES + 3u]);

    epd_write_cmd(0x3A); /* Dummy Line Period */
    epd_write_data(lut[EPD_LUT_BYTES + 4u]);
    epd_write_cmd(0x3B); /* Gate Line Width */
    epd_write_data(lut[EPD_LUT_BYTES + 5u]);

    epd_write_cmd(0x32); /* Write LUT Register - the payload, without the trailer */
    epd_write_data_buf(tbl, EPD_LUT_BYTES);
}
#endif

#if EPD_RESAMPLE_PER_REFRESH

void epd_resample_temperature(void)
{
    /* 0xB1 = enable clock, load temperature, load LUT - and notably NOT
     * display, so this is safe immediately before pushing a frame. */
    epd_write_cmd(0x18);
    epd_write_data(0x80);   /* the controller's internal sensor */

    epd_write_cmd(0x22);
#if EPD_INIT_FROM_OTP
    /* 0xB1 - load the temperature AND the waveform that goes with it. Not
     * negotiable on this path: reloading the LUT for the new temperature is
     * the entire point, and 0xA1 would leave the tag on whatever waveform it
     * booted with while still reporting a fresh number. */
    epd_write_data(0xB1);
#elif EPD_TEMP_LOAD_NOLUT
    epd_write_data(0xA1);   /* temperature only; our hand-written LUT stands */
#else
    epd_write_data(0xB1);   /* takes the OTP LUT with it - restored below */
#endif
    epd_write_cmd(0x20);    /* Master Activation */
    epd_wait_busy();

#if EPD_TEMP_READ
    (void)epd_read_temperature();
#endif

#if !EPD_INIT_FROM_OTP && !EPD_TEMP_LOAD_NOLUT
    /* The load above also pulled the OTP waveform in over the hand-written
     * one, because 0xB1 asks for both. Put ours back.
     *
     * This is why sampling on this path looked impossible at first, and it is
     * not: the LUT is 70 bytes plus five small registers, which is microseconds
     * of SPI against a refresh measured in seconds. Restoring is simply
     * cheaper than avoiding.
     *
     * There may be a cheaper way still - if 0x22 bit 5 (load temperature) and
     * bit 4 (load LUT) really are independent, then 0xA1 would sample without
     * touching the waveform and this rewrite would be unnecessary. That is a
     * datasheet claim we have never verified on this silicon, and the vendor's
     * driver only ever sends 0xB1, so it is not assumed here. Worth testing:
     * if a 0xA1 build keeps the Waveshare refresh duration, the bits split.
     *
     * Always the full-refresh table: nothing calls epd_init(false) today. */
    epd_load_waveshare_lut(epd_lut_full);
#endif
}

#endif  /* EPD_RESAMPLE_PER_REFRESH */

#if EPD_TEMP_SWEEP

volatile int8_t   epd_sweep_asked[EPD_SWEEP_N];
volatile int8_t   epd_sweep_echo[EPD_SWEEP_N];
volatile uint16_t epd_sweep_ms[EPD_SWEEP_N];
volatile uint8_t  epd_sweep_done;

/* Tell the controller it is `c` degrees, whatever the sensor thinks.
 *
 * 0x18 <- 0x48 is external-temperature mode: the controller stops sampling
 * its own sensor and uses the register instead. 0x1A then writes that
 * register, and 0x22 <- 0xB1 makes it act on the new value - the same load
 * the OTP init path does, so the waveform is reselected for the temperature
 * we just invented.
 *
 * Two bytes go to 0x1A because the register is 12-bit: whole degrees in the
 * first, fraction in the top nibble of the second. Only the first is
 * interesting, and only the first is what the retail firmware reads back.
 *
 * Unlike the read side, this half is NOT something we have seen the vendor
 * do - its driver only ever reads. 0x48 and the two-byte write are the
 * conventional counterparts of what we did verify, which is why every step
 * reads the value back: if the echo does not follow, the forcing is not
 * working and no timing below means anything.
 */
static void epd_force_temperature(int8_t c)
{
    epd_write_cmd(0x18);
    epd_write_data(0x48);        /* external / register-supplied */

    epd_write_cmd(0x1A);
    epd_write_data((uint8_t)c);  /* whole degrees, signed */
    epd_write_data(0x00);        /* fraction, top nibble - unused */

    epd_write_cmd(0x22);
    epd_write_data(0xB1);        /* load temperature + LUT, do not display */
    epd_write_cmd(0x20);
    epd_wait_busy();
}

/* Time one refresh at a forced temperature, in milliseconds.
 *
 * Deliberately blocking and deliberately not the app's 50 ms poll timer: this
 * is measuring, and 50 ms of quantisation is coarse next to the ~650 ms
 * difference two panels showed at the same temperature. The watchdog is
 * reloaded the way epd_wait_busy() does, since a refresh outlasts it. */
static uint16_t epd_time_one_refresh(void)
{
    uint16_t ms = 0;

    epd_display_start(epd_framebuffer);

    /* Cap well past any plausible waveform so a panel that never releases
     * BUSY ends the step instead of the sweep. */
    while (epd_display_busy() && ms < 10000u) {
        epd_delay_ms(1);
        ms++;
        wdg_reload(0xFF);
    }
    return ms;
}

void epd_temp_sweep(void)
{
    uint32_t i;

    epd_sweep_done = 0;

    for (i = 0; i < EPD_SWEEP_N; i++) {
        int8_t t = (int8_t)(EPD_SWEEP_FIRST + (int)i * EPD_SWEEP_STEP);
        uint32_t b;

        epd_force_temperature(t);

        epd_sweep_asked[i] = t;

        /* Straight back out of the register, so the timing on this row can be
         * trusted or discarded on its own merits. */
        epd_write_cmd(0x1B);
        epd_sweep_echo[i] = (int8_t)epd_read_byte(INPUT);

        /* Invert the framebuffer between steps so every refresh has real work
         * to do and the panel visibly counts through the sweep. A full update
         * runs the whole waveform regardless of content, but identical frames
         * would make it impossible to tell progress from a wedge. */
        for (b = 0; b < EPD_BUF_SIZE; b++) {
            epd_framebuffer[b] = (uint8_t)~epd_framebuffer[b];
        }

        epd_sweep_ms[i] = epd_time_one_refresh();
    }

    /* Hand the controller back its own sensor, so whatever runs afterwards is
     * not stuck on the last value we invented. */
    epd_write_cmd(0x18);
    epd_write_data(0x80);
    epd_write_cmd(0x22);
    epd_write_data(0xB1);
    epd_write_cmd(0x20);
    epd_wait_busy();

    epd_sweep_done = 1;
}

#endif  /* EPD_TEMP_SWEEP */

#if EPD_LUT_PROBE

/* Big enough to be unmistakable in a phase duration, small enough that a hit
 * does not take a visible age - 161 of them run back to back. */
#define LUT_PROBE_MARK 0x10u

volatile uint16_t epd_lut_probe_ms[EPD_LUT_PROBE_LEN + 1u];
volatile uint16_t epd_lut_probe_done;

/* Write the payload, trigger an update, and time it.
 *
 * 0xC7 and not 0xF7: bit 4 clear means "display with the LUT that is resident",
 * which is the one just written. 0xF7 would reload the OTP table over it and
 * every offset would return the same number - the measurement would look like a
 * flat register rather than a broken command.
 *
 * No RAM write either. This measures a waveform's duration and nothing else. */
static void epd_lut_probe_one(uint16_t index, uint16_t offset, bool marked)
{
    static uint8_t payload[EPD_LUT_PROBE_LEN];
    uint16_t i;
    uint16_t ms = 0;

    for (i = 0; i < EPD_LUT_PROBE_LEN; i++) {
        payload[i] = 0u;
    }
    if (marked) {
        payload[offset] = LUT_PROBE_MARK;
    }

    epd_write_cmd(0x32);
    epd_write_data_buf(payload, EPD_LUT_PROBE_LEN);

    epd_write_cmd(0x22);
    epd_write_data(0xC7);
    epd_write_cmd(0x20);

    /* Same blocking millisecond loop the temperature sweep uses, and for the same
     * reason: this is measuring, and the app's 50 ms poll would quantise away the
     * differences being looked for. */
    while (epd_display_busy() && ms < 10000u) {
        epd_delay_ms(1);
        ms++;
        wdg_reload(0xFF);
    }
    epd_lut_probe_ms[index] = ms;
}

void epd_lut_probe(void)
{
    uint16_t off;

    epd_lut_probe_done = 0;

    /* The control first, so a sweep that raises nothing anywhere can still be
     * told apart from a sweep that never ran at all. */
    epd_lut_probe_one(EPD_LUT_PROBE_LEN, 0, false);

    for (off = 0; off < EPD_LUT_PROBE_LEN; off++) {
        epd_lut_probe_one(off, off, true);
        epd_lut_probe_done = (uint16_t)(off + 1u);
    }

    /* Deliberately never returns. The results are the whole output and the BLE
     * stack was never started, so there is nothing to go back to - and stopping
     * here keeps the panel from being repainted over the state that was measured. */
    epd_sleep();
    for (;;) {
        wdg_reload(0xFF);
    }
}

#endif  /* EPD_LUT_PROBE */

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

/* epd_read_byte() now lives above, shared with EPD_TEMP_READ. It was written
 * for this probe and is unchanged in behaviour on this variant.
 *
 * MEASURED 2026-08-09, AND THIS FUNCTION LIES ON VARIANT A. Both variant-A tags
 * (Type 3, `SLH1910` and `SLH1940`) return 0x00 to cmd 0x2F with no pull and
 * 0xFF with the pull-up, i.e. the controller does not drive the line for that
 * command at all - while cmd 0x1B on the same pad, same bit loop, returns a
 * correct temperature. So the command is simply unsupported on those panels,
 * and this function reports a healthy panel as ABSENT.
 *
 * Left as it is, because nothing acts on the result and the two raw bytes are
 * still worth having. But do not wire it into a decision, and do not read
 * "absent" as absent without checking epd_probe_pullup/pulldown by hand: 0xFF
 * against 0x00 is this, not a loose flex. See hema-local/docs/PANEL_LOTS.md. */

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

#if EPD_PANEL_ID

volatile uint8_t epd_panel_id_status;
volatile uint8_t epd_panel_id_user[EPD_PANEL_ID_LEN];
volatile uint8_t epd_panel_id_option[EPD_PANEL_ID_LEN];
volatile uint8_t epd_panel_id_status_pu;
volatile uint8_t epd_panel_id_user_pu[EPD_PANEL_ID_LEN];
volatile uint8_t epd_panel_id_option_pu[EPD_PANEL_ID_LEN];
volatile uint8_t epd_panel_id_done;

/* Clock `len` bytes out of whichever register the preceding command selected,
 * holding CS low across the whole transaction.
 *
 * That is the reason this is not just a loop over epd_read_byte(): that one
 * raises CS per byte, and on this controller CS framing is what delimits a
 * transaction - so a loop would restart the read and hand back byte 0 `len`
 * times over, which is a failure that looks exactly like a register full of
 * one repeated value. */
static void epd_read_block(volatile uint8_t *dst, uint8_t len, uint32_t pull)
{
    epd_read_begin(pull);
    for (uint8_t i = 0; i < len; i++) {
        dst[i] = epd_read_bits();
    }
    epd_read_end();
}

/* Issue the command, then read it - twice, with different pulls. The command
 * has to be re-issued for the second read because it is what selects the
 * register and starts it shifting; reading again without it would continue past
 * the end rather than start over. */
static void epd_read_reg(uint8_t cmd, volatile uint8_t *plain,
                         volatile uint8_t *pulled, uint8_t len)
{
    epd_write_cmd(cmd);
    epd_read_block(plain, len, INPUT);

    epd_write_cmd(cmd);
    epd_read_block(pulled, len, INPUT_PULLUP);
}

void epd_panel_read_id(void)
{
    epd_read_reg(0x2F, &epd_panel_id_status, &epd_panel_id_status_pu, 1u);
    epd_read_reg(0x2E, epd_panel_id_user, epd_panel_id_user_pu,
                 EPD_PANEL_ID_LEN);
    epd_read_reg(0x2D, epd_panel_id_option, epd_panel_id_option_pu,
                 EPD_PANEL_ID_LEN);

    epd_panel_id_done = 1u;
}

#endif  /* EPD_PANEL_ID */

void epd_init(bool full_lut)
{
#if EPD_PARTIAL
    /* Both paths below issue SWRESET, which clears the controller's RAM. So
     * whatever the shadow claims the panel is holding stops being true here, and
     * the next refresh has to be a full one to establish a base again. */
    epd_display_forget();
#endif
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

    /* Select the sensor, load the temperature, load the OTP waveform for it -
     * and read the value back if we were built to. Shared with every later
     * refresh, which repeats it so the waveform tracks the temperature rather
     * than being frozen at whatever it was when the tag booted. */
    epd_resample_temperature();
#if EPD_TEMP_SWEEP
    /* Last thing in init, so the panel and the bus are fully up and the sweep
     * measures refreshes rather than bring-up. Blocks for about a minute. */
    epd_temp_sweep();
#endif
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

        epd_load_waveshare_lut(lut);

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

#if EPD_PANEL_ID
    /* Last, on both paths: the controller is fully configured and awake by here,
     * which is what an OTP read wants, and the read writes nothing so it cannot
     * disturb whatever was just set up. */
    epd_panel_read_id();
#endif

#if EPD_LUT_PROBE
    /* Truly last, because it never comes back. Deliberately outside the path
     * split above so it can run on either waveform: on an OTP panel it is the
     * measurement, and on one that accepts the Waveshare table it is the control
     * - the timing region should show up at bytes 35 to 70, which tests the probe
     * rather than the panel. */
    epd_lut_probe();
#endif
}

bool epd_display_busy(void)
{
    /* BUSY is active-high while busy, confirmed by the Waveshare reference. */
    return GPIO_GetPinStatus(EPD_PIN(BUSY)) ? true : false;
}

/* Display Update Control 2 payloads.
 *
 * The bits, which is what makes these values readable rather than magic:
 *
 *   7 enable clock   6 enable analog   5 load temperature   4 load LUT
 *   3 Display Mode 2   2 display   1 disable analog   0 disable clock
 *
 * So 0xC7 = power up, display, power down. Measured; it is what every working
 * tag has used since bring-up, and note that it leaves the analog and the clock
 * OFF when it finishes.
 *
 * That last part cost a wedged panel. 0x0C - Waveshare's own partial value -
 * is mode 2 plus display and *no enable bits at all*, because their flow powers
 * the panel up separately in its partial init (0x22 <- 0xC0, visible in
 * epd_init()'s partial branch below) and leaves it powered. We never call that
 * branch, so 0x0C asked a powered-down controller to refresh: it raised BUSY and
 * never lowered it, epd_wait_busy() then burned several seconds on every
 * subsequent refresh, and the BLE link dropped each time from the starved main
 * loop. Diagnosed 2026-08-09 from s_poll_count 200 with epd_temp_c reading 0.
 *
 * So the partial values are self-contained, powering up and down exactly as the
 * full one does:
 *
 * The two paths need OPPOSITE treatment of bit 4, the load-LUT bit, and getting
 * that backwards is what made the OTP partial silently run a full waveform.
 *
 *   Waveshare  0xC7, identical to full. Bit 4 clear, i.e. "display with whatever
 *              LUT is resident" - and epd_select_waveform() has just written the
 *              partial table by hand with 0x32, so resident is exactly what we
 *              want. Bit 3 stays clear too: mode 2 would pick an OTP waveform
 *              over the table we just wrote.
 *   OTP        0xFF, i.e. 0xF7 plus Display Mode 2. Bit 4 SET, because here there
 *              is no hand-written table and the OTP waveform has to be loaded -
 *              and the mode bit is what chooses which of the panel's two
 *              waveforms gets loaded. A mode bit on a command that loads nothing
 *              selects nothing.
 *
 * That last sentence was learned twice. 0xCF (0xC7 plus the mode bit) was tried
 * first and produced a full refresh every time: bit 4 clear meant no LUT was
 * loaded, so the panel displayed with whatever was resident - which
 * epd_resample_temperature() had loaded moments earlier as 0xB1, the mode-1 full
 * waveform. Measured on the SLH1904 tag as epd_last_paint 1 with s_poll_count 73,
 * that panel's exact full-refresh figure. The Type 5 notes already recorded that
 * 0xF7 loads the OTP LUT while 0xC7 displays without reloading it; the partial
 * value was built on the wrong one of those. */
#define EPD_UPD_FULL     0xC7u
#if EPD_INIT_FROM_OTP
#define EPD_UPD_PARTIAL  0xFFu
#else
#define EPD_UPD_PARTIAL  0xC7u
#endif

#if EPD_PARTIAL

/* What the panel is believed to be showing, and how much has been done to it
 * since the last clean sweep. A belief, not a fact - see epd_display_forget(). */
static uint8_t   s_shadow[EPD_BUF_SIZE];
static bool      s_shadow_valid;
volatile uint8_t epd_partial_run;
volatile uint8_t epd_last_paint;

void epd_display_forget(void)
{
    s_shadow_valid = false;
}

/* Install the waveform this refresh wants.
 *
 * The uncertain half of partial refresh, kept in one place for that reason. */
static void epd_select_waveform(bool partial)
{
#if EPD_INIT_FROM_OTP
    /* Nothing to send. The waveform lives in the panel's OTP and which of the
     * two gets used is chosen by the Display Mode bit of 0x22, below. This is
     * the whole reason the OTP path is the one to trust: the panel supplies a
     * partial waveform calibrated for its own lot, and we never have to know
     * what it looks like. */
    (void)partial;
#else
    /* Two hand-written tables, installed the same way. epd_load_waveshare_lut()
     * hardcodes VCOM 0x55, which is the full-refresh value; Waveshare's partial
     * sequence uses 0x26, so it is applied afterwards rather than by threading a
     * second parameter through a function every other caller wants unchanged.
     *
     * UNVERIFIED, and the partial table is thin - a single 10-frame group where
     * the full one has 60. A lot that takes the full table should take this one
     * (same controller, same LUT format) but 10 frames is very little drive, and
     * shape-compatible is not the same as working. */
    epd_load_waveshare_lut(partial ? epd_lut_partial : epd_lut_full);
    if (partial) {
        epd_write_cmd(0x2C);        /* Write VCOM Register, partial value */
        epd_write_data(0x26);
    }
#endif
}

#endif  /* EPD_PARTIAL */

/* Point the RAM window and its address counters at a band of rows, then stream
 * those rows into `ram_cmd`.
 *
 * The window is set on every write rather than inherited from epd_init(). It has
 * to be: a partial refresh narrows the Y window, and a later full refresh that
 * assumed epd_init()'s bounds would then repaint only the old band and leave the
 * rest of the panel stale - a bug that would appear one refresh after the one
 * that caused it.
 *
 * X is always the full width. Restricting it would save a few bytes of SPI and
 * nothing else, because refresh time is set by how many gate lines are driven.
 */
static void epd_set_window(uint16_t first, uint16_t last)
{
    epd_write_cmd(0x44);    /* RAM X window */
    epd_write_data(0x00);
    epd_write_data((EPD_WIDTH_BYTES - 1) & 0xFF);

    epd_write_cmd(0x45);    /* RAM Y window - the band */
    epd_write_data(first & 0xFF);
    epd_write_data((first >> 8) & 0xFF);
    epd_write_data(last & 0xFF);
    epd_write_data((last >> 8) & 0xFF);

    /* Counters to the top of the band. Y counts UP - see the Data Entry Mode
     * note in epd_init(); starting at the far end would mirror the image. */
    epd_write_cmd(0x4E);
    epd_write_data(0x00);
    epd_write_cmd(0x4F);
    epd_write_data(first & 0xFF);
    epd_write_data((first >> 8) & 0xFF);
}

static void epd_write_rows(const uint8_t *fb, uint16_t first, uint16_t last,
                           uint8_t ram_cmd)
{
    uint32_t i;
    uint32_t from = (uint32_t)first * EPD_WIDTH_BYTES;
    uint32_t to   = ((uint32_t)last + 1u) * EPD_WIDTH_BYTES;

    epd_set_window(first, last);

    /* Our framebuffer uses 1 = white (matching the vendor's DSL where color 1 =
     * white, and the vendor image encoder), but the panel RAM is the opposite
     * polarity - Waveshare's own EPD_2IN13_V2_Display sends ~Image for exactly
     * this reason. So invert on the way out. If the very first test pattern
     * comes out as white-on-black, flip EPD_INVERT_OUTPUT to 0 and reflash. */
    epd_write_cmd(ram_cmd);
    GPIO_SetActive(EPD_PIN(DC));   /* DC high = data */
    epd_cs_low();
#if EPD_TX_PROFILE
    /* SysTick counts DOWN from its reload at a 1 MHz reference, so the
     * difference is microseconds directly. Started here rather than left
     * running because epd_delay_ms() drives the same timer and would reset it
     * under us; nothing inside this loop delays. */
    systick_start(0xFFFFFFu, 0);
    /* systick_start() zeroes VAL and the counter does not reload until the
     * next tick, so an immediate read can legitimately return 0 and make the
     * difference below meaningless - it read as 4278265734 us the first time.
     * Wait for the reload before taking the start point. */
    while (systick_value() == 0u) { }
    epd_tx_t0 = systick_value();
#endif
    for (i = from; i < to; i++) {
        uint8_t b = fb[i];
#if EPD_INVERT_OUTPUT
        b = (uint8_t)~b;
#endif
        epd_tx(&b, 1);
    }
#if EPD_TX_PROFILE
    /* Masked to 24 bits: SysTick's counter is that wide, so a plain 32-bit
     * subtraction of two reads is only right by accident. */
    epd_tx_us    = (epd_tx_t0 - systick_value()) & 0xFFFFFFu;
    epd_tx_bytes = (uint32_t)(to - from);
    systick_stop();
#endif
    epd_cs_high();
}

epd_paint_t epd_display_start(const uint8_t *framebuffer)
{
    epd_paint_t did = EPD_PAINT_FULL;
    uint16_t first  = 0;
    uint16_t last   = EPD_HEIGHT - 1;

#if EPD_PARTIAL
    if (s_shadow_valid) {
        if (!epd_gfx_dirty_rows(s_shadow, framebuffer, &first, &last)) {
            /* Identical to the glass. Sending it would cost a refresh and a
             * visible flash to produce the picture already there. */
            epd_last_paint = (uint8_t)EPD_PAINT_NONE;
            return EPD_PAINT_NONE;
        }

        if (epd_partial_run < EPD_PARTIAL_RUN_MAX &&
            (uint16_t)(last - first + 1u) <= EPD_PARTIAL_MAX_ROWS) {
            did = EPD_PAINT_PARTIAL;
        } else {
            first = 0;
            last  = EPD_HEIGHT - 1;
        }
    }

    epd_select_waveform(did == EPD_PAINT_PARTIAL);
#endif

#if EPD_PARTIAL
    /* Both banks, over the WHOLE panel, then the window narrowed just before the
     * update. That combination is deliberate and it is what fixes ghosting
     * appearing outside the band.
     *
     * The reason is an unknown we do not have to resolve: setting the RAM window
     * certainly restricts where RAM writes land, but whether the controller also
     * restricts the GATE SCAN to that window during an update is not something we
     * have established on this silicon. The evidence from 2026-08-09 says it does
     * not - as soon as partials began, residue from many earlier frames appeared
     * across the entire screen, which cannot happen if untouched rows are never
     * driven.
     *
     * If the whole panel is scanned, then every pixel is driven through the
     * partial LUT, and that LUT moves a pixel only where 0x24 and 0x26 disagree
     * (its black-to-black and white-to-white groups are zero). So the invariant
     * that matters is: **outside the changed band, 0x24 must equal 0x26.** Writing
     * both banks in full guarantees it, because rows outside the band are by
     * definition identical between the old frame and the new one.
     *
     * Writing only the band, as this did before, left 0x26 outside it holding
     * whatever it happened to hold - frames from an arbitrary distance in the past
     * - and a full-panel scan then faithfully drove all of that back onto the
     * glass. Hence "residue from many previous refreshes", and hence why a full
     * refresh cleaned it and the next partial brought it all back.
     *
     * The narrowing afterwards costs two commands and keeps the gate-line saving
     * available if the scan does turn out to honour the window. Correct either
     * way; fast if we are lucky.
     *
     * Cost of the full writes over banded ones: ~8 ms of SPI against a refresh
     * measured in hundreds. Not worth optimising against an unknown.
     *
     * 0x26 = the frame being REPLACED, written before the new one goes into 0x24.
     *
     * There are two comparisons in a partial refresh and they are easy to
     * conflate. epd_gfx_dirty_rows() above runs on this CPU against the shadow,
     * and decides how many ROWS to touch. The controller then does its own
     * comparison, per pixel, between 0x24 and 0x26, and that is what decides
     * which pixels actually move - the partial LUT only drives the black-to-white
     * and white-to-black groups, leaving unchanged pixels alone. That is the
     * whole point of a partial waveform, and it means 0x26 must hold what the
     * glass is currently showing or the wrong pixels are left behind.
     *
     * Getting this wrong is what put a 9 on top of an 8 on 2026-08-09. Only the
     * full path seeded 0x26, so every partial compared against the frame from the
     * last FULL refresh: pixels differing from that were driven correctly, and
     * pixels that happened to agree with it kept whatever the intervening frames
     * had left there. The band was right the whole time - it is the per-pixel
     * comparison that was against a stale frame.
     *
     * Written explicitly rather than relying on the controller to copy 0x24 into
     * 0x26 when an update finishes. Some SSD16xx parts do; whether this one does
     * in the mode we drive it in is exactly the kind of thing we would be
     * guessing at, and one extra windowed write is ~4 ms of SPI.
     *
     * Nothing else depends on 0x26's contents, so there is deliberately no
     * attempt to keep it meaningful between refreshes - a full refresh drives
     * every pixel regardless (its LUT drives all four transition groups, not just
     * the two that change), so it needs no base at all. */
    if (did == EPD_PAINT_PARTIAL) {
        epd_write_rows(s_shadow, 0, EPD_HEIGHT - 1, 0x26);
    }
#endif

    epd_write_rows(framebuffer,
#if EPD_PARTIAL
                   0, EPD_HEIGHT - 1,
#else
                   first, last,
#endif
                   0x24);

#if EPD_PARTIAL
    /* Now narrow the scan to the band, having filled both banks in full. */
    if (did == EPD_PAINT_PARTIAL) {
        epd_set_window(first, last);
    }
#endif

    epd_write_cmd(0x22); /* Display Update Control 2 */
    epd_write_data(did == EPD_PAINT_PARTIAL ? EPD_UPD_PARTIAL : EPD_UPD_FULL);
    epd_write_cmd(0x20); /* Master Activation */

#if EPD_PARTIAL
    /* Recorded now rather than when the refresh finishes, which is a bet that the
     * refresh will finish. It usually will, and committing here keeps the
     * completion path free of driver state.
     *
     * The bet has to be paid off when it loses, though: a refresh that times out
     * leaves the glass showing something other than what this says, and every
     * later diff would be against a frame that was never displayed - stale rows
     * silently never repainted. So the caller calls epd_display_forget() on
     * timeout. Do not remove that without moving this. */
    memcpy(s_shadow, framebuffer, EPD_BUF_SIZE);
    s_shadow_valid = true;
    epd_partial_run = (did == EPD_PAINT_PARTIAL)
                    ? (uint8_t)(epd_partial_run + 1u) : 0u;
    epd_last_paint = (uint8_t)did;
#endif

    /* Deliberately no epd_wait_busy() here. The panel drives BUSY high and
     * refreshes on its own for ~2 s; the caller polls epd_display_busy() from
     * a timer so the BLE stack keeps getting scheduled meanwhile. Everything
     * above is just SPI - about 4 ms for the 4000-byte RAM write at 8 MHz -
     * so returning now costs the link nothing. */
    return did;
}

void epd_sleep(void)
{
#if EPD_PARTIAL
    /* Deep sleep drops the controller's RAM, and 0x10 is only left by a hardware
     * reset, which clears it too. Either way the base image is gone. */
    epd_display_forget();
#endif
    epd_write_cmd(0x22); /* POWER OFF */
    epd_write_data(0xC3);
    epd_write_cmd(0x20);

    epd_write_cmd(0x10); /* Deep Sleep Mode */
    epd_write_data(0x01);
    epd_delay_ms(100);
}
