/**
 * epd_ssd1680.h
 *
 * SSD1680-family e-paper controller driver for the Hema 2.13" ESL tag.
 *
 * Command sequence and LUT usage were reverse-engineered from the community
 * "5_hema_clock_down_*_V1.57.bin" firmware (see PROTOCOL_NOTES.md at the repo
 * root) and cross-checked against the panel vendor's own reference driver,
 * which is itself explicitly named after (i.e. ported from) Waveshare's public
 * EPD_2IN13_V2 driver for the same SSD1680-family controller.
 *
 * Panel identified (see PROTOCOL_NOTES.md section 2):
 *   low-res:  Good Display HINK-E0213A41-FPC / HINK-E0213A07-A1, 104 x 212 px
 *   high-res: Good Display HINK-E0213A53-FPC-A0,                122 x 250 px
 *
 * STATUS: verified on real hardware. The pin map below was recovered from the
 * community firmware, confirmed by continuity-testing the EPD FPC connector
 * back to the DA14585 package, and then proven end-to-end by rendering a test
 * pattern on the panel. Two places where this deliberately DIVERGES from
 * Waveshare's reference driver are marked in epd_ssd1680.c and were each
 * confirmed by photographing the result:
 *   - EPD_INVERT_OUTPUT is 0 (Waveshare inverts; its GUI layer uses the
 *     opposite 1=black convention to ours)
 *   - Data Entry Mode is 0x03 / Y-increment (Waveshare uses 0x01 /
 *     Y-decrement, which mirrors our top-down framebuffer vertically)
 */

#ifndef _EPD_SSD1680_H_
#define _EPD_SSD1680_H_

#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"

/* ------------------------------------------------------------------------
 * Panel geometry. This board's panel is HINK-E0213A53-FPC-A0 = the high-res
 * 122x250 variant (confirmed from the FPC label in the board photo), so
 * high-res is the default. To build for the low-res 104x212 panel instead,
 * define EPD_PANEL_LOW_RES project-wide.
 * ---------------------------------------------------------------------- */
#if defined(EPD_PANEL_LOW_RES)
    #define EPD_WIDTH   104
    #define EPD_HEIGHT  212
#else
    #define EPD_WIDTH   122
    #define EPD_HEIGHT  250
#endif

/* Bytes per row in the 1bpp framebuffer (width rounded up to a byte). */
#define EPD_WIDTH_BYTES  ((EPD_WIDTH + 7) / 8)
#define EPD_BUF_SIZE     (EPD_WIDTH_BYTES * EPD_HEIGHT)

/* ------------------------------------------------------------------------
 * Which init sequence the panel gets.
 *
 * Two exist, and the axis that chooses between them is the BOARD, not the
 * panel size:
 *
 *   OTP        the sequence the retail firmware uses. Writes no waveform at
 *              all - cmd 0x18 selects the internal temperature sensor and cmd
 *              0x22 bit 4 loads the factory LUT out of the controller's OTP.
 *   WAVESHARE  a hand-written 70-byte LUT via cmd 0x32, from Waveshare's
 *              EPD_2IN13_V2 reference. Proven on the Type 1 board.
 *
 * This was gated on panel resolution when the A41 was first driven, which
 * happened to work but was the wrong reading. In the retail firmware the
 * 104x212 and 122x250 panel descriptors register the *same* vtable - the
 * constructor at 0x07FC3BF6, whose init is 0x07FC399E - so that one sequence
 * drives both sizes. What it does not cover is variant B, which is the board
 * we have only ever driven with the Waveshare LUT.
 *
 * So: variant A takes the sequence its own firmware ships with, at whatever
 * geometry; variant B keeps the one proven on it. Set EPD_INIT_FROM_OTP
 * explicitly to override.
 * ---------------------------------------------------------------------- */
#if !defined(EPD_INIT_FROM_OTP)
    #if defined(EPD_BOARD_VARIANT_A)
        #define EPD_INIT_FROM_OTP 1
    #else
        #define EPD_INIT_FROM_OTP 0
    #endif
#endif

/* Build the panel-presence probe in. Off by default: it is a bring-up tool for
 * a tag whose screen will not move, not something a working tag needs, and it
 * is ~240 bytes of flash that a shipping image should not carry. Turn it on in
 * user_config.h when a panel is silent - see epd_panel_present(). */
#if !defined(EPD_PANEL_PROBE)
    #define EPD_PANEL_PROBE 0
#endif

/* Build in the controller's temperature reading. It feeds {T}, and on the OTP
 * path it also reports the value the controller picked its waveform with.
 *
 * Note the vendor's own driver only changes behaviour BELOW 10 C - see
 * epd_read_temperature() - and the panel's OTP curve is flat above ~30 C, so
 * between those a healthy sensor and a stuck one look identical from the
 * outside. Read the number; do not infer it from how long a refresh took.
 *
 * Works on BOTH waveform paths, but defaults on only for OTP.
 *
 * Sampling means cmd 0x22 = 0xB1, which loads the temperature and the OTP
 * waveform together. On the OTP path that is exactly what we want anyway. On
 * the Waveshare path it lands on top of the hand-written LUT - so that path
 * writes the LUT back afterwards, which costs 70 bytes plus five registers of
 * SPI, microseconds against a refresh of seconds. It is not impossible there,
 * just not free.
 *
 * The default differs because the VALUE differs. An OTP build needs the
 * reading to keep its waveform matched to the temperature, so it pays for
 * itself. A Waveshare build gets nothing from it but the number, since that
 * LUT is fixed and temperature-independent - so it is opt-in there, and the
 * cost is perturbing a working waveform once per refresh rather than the ~250
 * bytes of code.
 *
 * Turn it on for a fast tag that should display {T}:
 *     tools/build.sh --type 4 --fast --temp
 */
#if !defined(EPD_TEMP_READ)
    #define EPD_TEMP_READ EPD_INIT_FROM_OTP
#endif

/* Whether a refresh re-samples at all. Two independent reasons: an OTP build
 * must, to keep the waveform matched to the current temperature; any build
 * that reports a temperature must, or {T} would show the boot-time value for
 * the life of the boot. */
#define EPD_RESAMPLE_PER_REFRESH  (EPD_INIT_FROM_OTP || EPD_TEMP_READ)

/* Map the panel's OTP waveform against temperature, by lying to the
 * controller about how warm it is.
 *
 * Nothing we can disassemble answers this: the firmware does not choose a
 * waveform, it delegates (cmd 0x22 bit 4 loads the LUT the controller picks
 * for itself), so the table lives in the PANEL's OTP and not in any image we
 * hold. The only way to see it is from the outside.
 *
 * So cmd 0x18 is switched to external-temperature mode and cmd 0x1A supplies
 * the number, which lets a whole range be swept in a minute without a fridge
 * or a hairdryer. Each step forces a temperature, refreshes, and records how
 * long the panel took. A STEP in those durations is an OTP band boundary.
 *
 * This is a bench build, not something to ship: it blocks for the whole sweep
 * during init and it scribbles on the framebuffer. Implies EPD_TEMP_READ. */
#if !defined(EPD_TEMP_SWEEP)
    #define EPD_TEMP_SWEEP 0
#endif

#if EPD_TEMP_SWEEP && !EPD_TEMP_READ
    #undef  EPD_TEMP_READ
    #define EPD_TEMP_READ 1
#endif

/* ------------------------------------------------------------------------
 * BOARD VARIANT — set exactly one.
 *
 * Two ESL boards exist carrying the same DA14585 and the same SSD1680-family
 * panel, wired differently. The community firmware chose between them at
 * runtime from a stored config byte; we choose at build time.
 *
 *   VARIANT B  the first tag studied here. Pin map recovered from the
 *              community firmware and confirmed by continuity-testing the FPC
 *              connector back to the package.
 *   VARIANT A  the tag that arrived still running the original retail
 *              firmware. Pin map read straight out of that firmware's own
 *              live pin table at 0x07FD4428 in a SysRAM dump — see
 *              hema-local/re/newtag/README.md. Only CS is shared with B.
 *
 * GETTING THIS WRONG IS INVISIBLE AT BOOT. The tag starts, advertises and
 * accepts commands exactly as normal; the panel simply never changes. That is
 * precisely what happened when a variant-B build was flashed to the variant-A
 * tag, so if a board goes quiet on the panel alone, check this first.
 * ---------------------------------------------------------------------- */
#if !defined(EPD_BOARD_VARIANT_A) && !defined(EPD_BOARD_VARIANT_B)
    /* Default: the variant-A tag, which is the one on the bench. */
    #define EPD_BOARD_VARIANT_A
#endif

#if defined(EPD_BOARD_VARIANT_A)

/* ------------------------------------------------------------------------
 * VARIANT A pin map, read from the retail firmware's own table.
 *
 *   EPD signal   DA14585 GPIO   table entry
 *   ----------   ------------   -----------
 *   SCK            P0_1           +0x06
 *   SDA (MOSI)     P2_0           +0x08
 *   D/C            P0_7           +0x0A
 *   CS             P2_1           +0x00
 *   RST            P1_0           +0x04   (the only entry ever pulsed low)
 *   BUSY (input)   P1_1           +0x0C
 *   enable         P2_3           +0x0E   (held high)
 *   enable         P2_2           +0x02   (held high — note B drives it LOW)
 *
 * The panel is bit-banged here rather than driven by the hardware SPI block,
 * following the retail firmware, which bit-bangs these same two pins. It costs
 * nothing on this board and buys a real simplification: the panel pins are
 * disjoint from the boot flash's (P0_0/P0_3/P0_5/P0_6), so unlike variant B
 * there is no bus to share and no D/C-versus-MISO collision on P0_5.
 * ---------------------------------------------------------------------- */
#define EPD_BITBANG      1

#define EPD_SCK_PORT     GPIO_PORT_0
#define EPD_SCK_PIN      GPIO_PIN_1

#define EPD_SDA_PORT     GPIO_PORT_2
#define EPD_SDA_PIN      GPIO_PIN_0

#define EPD_DC_PORT      GPIO_PORT_0
#define EPD_DC_PIN       GPIO_PIN_7

#define EPD_RST_PORT     GPIO_PORT_1
#define EPD_RST_PIN      GPIO_PIN_0

#define EPD_BUSY_PORT    GPIO_PORT_1
#define EPD_BUSY_PIN     GPIO_PIN_1

#define EPD_CS_PORT      GPIO_PORT_2
#define EPD_CS_PIN       GPIO_PIN_1

#define EPD_PWR_PORT     GPIO_PORT_2
#define EPD_PWR_PIN      GPIO_PIN_3

/* In variant A's retail pin table and held high there, so we hold it high too -
 * but on the Type 3 board this pad is not connected to anything: it runs to the
 * unpopulated resistor position R22 and stops. Do not read anything into its
 * state. (It was documented here as a "second enable line"; that was a guess,
 * and tracing the PCB disproved it.) */
#define EPD_AUX_PORT     GPIO_PORT_2
#define EPD_AUX_PIN      GPIO_PIN_2

#else   /* EPD_BOARD_VARIANT_B */

#define EPD_BITBANG      0

/* ------------------------------------------------------------------------
 * VARIANT B pin assignments — RECOVERED FROM THE COMMUNITY FIRMWARE.
 *
 * These are no longer guesses. They were extracted from the community
 * `5_hema_clock_down_high_V1.57.bin` by decompiling both the pin-setup
 * function and the actual command/data/reset routines with a correctly
 * memory-mapped Ghidra project, and the two agree (see PROTOCOL_NOTES.md
 * §13). The firmware supports two board variants selected by a stored
 * config byte; the values below are "variant B" (config byte == 0), which
 * is the fully cross-checked one and whose control pins sit on physically
 * adjacent package pins (7/8/9/10) — consistent with a real board layout.
 *
 *   EPD signal   DA14585 GPIO   QFN40 pin
 *   ----------   ------------   ---------
 *   SCK  (CLK)     P0_0            1
 *   SDA  (MOSI)    P0_6            9     (SPI_DO, see user_periph_setup.h)
 *   D/C            P0_5            7
 *   CS             P2_1            8     (SPI_EN, see user_periph_setup.h)
 *   RST            P0_7           10
 *   BUSY (input)   P2_0           40
 *   PWR-enable     P2_3           18    (driven high by the stock firmware)
 *   aux (unknown)  P2_2           13    (driven low by the stock firmware)
 *
 * SCK and MOSI here are the hardware SPI block's pads (see
 * user_periph_setup.h), shared with the boot flash.
 * ---------------------------------------------------------------------- */
#ifndef EPD_DC_PORT
#define EPD_DC_PORT      GPIO_PORT_0
#define EPD_DC_PIN       GPIO_PIN_5     /* QFN40 pin 7  */
#endif

#ifndef EPD_RST_PORT
#define EPD_RST_PORT     GPIO_PORT_0
#define EPD_RST_PIN      GPIO_PIN_7     /* QFN40 pin 10 */
#endif

#ifndef EPD_BUSY_PORT
#define EPD_BUSY_PORT    GPIO_PORT_2
#define EPD_BUSY_PIN     GPIO_PIN_0     /* QFN40 pin 40 */
#endif

/* Chip-select. Driven manually as a plain GPIO (the stock firmware bit-bangs
 * it too), NOT via the hardware SPI_EN function — so it must be configured
 * PID_GPIO. Physically confirmed on U4 connector pad 13. */
#ifndef EPD_CS_PORT
#define EPD_CS_PORT      GPIO_PORT_2
#define EPD_CS_PIN       GPIO_PIN_1     /* QFN40 pin 8  */
#endif

/* Panel power-enable line: the stock firmware drives P2_3 high at init.
 * Almost certainly gates the EPD's supply / booster — configure and assert
 * it or the panel may stay dark regardless of correct SPI. */
#ifndef EPD_PWR_PORT
#define EPD_PWR_PORT     GPIO_PORT_2
#define EPD_PWR_PIN      GPIO_PIN_3     /* QFN40 pin 18 */
#endif

/* Clock and data, named here only so a read can borrow them.
 *
 * Writing never touches these: on this variant both pads belong to the
 * hardware SPI block (SPI_CLK_PORT / SPI_DO_PORT in user_periph_setup.h, set
 * to PID_SPI_CLK / PID_SPI_DO), and epd_tx() goes through spi_send(). But the
 * panel's data line is bidirectional, and reading it means taking the pads
 * back as GPIOs for the turnaround - see epd_read_byte(). They must therefore
 * agree with user_periph_setup.h; they are the same two pins the table above
 * lists as SCK and SDA. */
#ifndef EPD_SCK_PORT
#define EPD_SCK_PORT     GPIO_PORT_0
#define EPD_SCK_PIN      GPIO_PIN_0     /* QFN40 pin 1  */
#endif

#ifndef EPD_SDA_PORT
#define EPD_SDA_PORT     GPIO_PORT_0
#define EPD_SDA_PIN      GPIO_PIN_6     /* QFN40 pin 9  */
#endif

#endif  /* board variant */

/* Which wiring this image was built for, as a string, so the built binary can
 * say so out loud. tools/flash.sh reads it back out of the .bin and refuses to
 * program a tag whose variant was not stated and matched. A wrong variant is
 * otherwise silent - the tag boots and advertises normally and only the panel
 * stays dead - and it has cost a working tag twice. */
#if defined(EPD_BOARD_VARIANT_A)
#define EPD_BOARD_VARIANT_STR   "A"
#else
#define EPD_BOARD_VARIANT_STR   "B"
#endif

/* Literal the flasher greps for. Kept as one contiguous string so it survives
 * into .rodata verbatim and cannot be confused with any other text. */
#define EPD_BOARD_VARIANT_TAG   "HEMA-BOARD-VARIANT-" EPD_BOARD_VARIANT_STR

/* The same again for the geometry, which the wiring stamp says nothing about.
 * Nothing used to check it, so a high-res image on a low-res tag passed the
 * flasher and turned up as a garbled panel - a wrong build presenting as a
 * hardware fault, which is the failure mode this whole apparatus exists to
 * stop. Built from EPD_WIDTH/EPD_HEIGHT so it cannot disagree with them. */
#define EPD__STR2(x)            #x
#define EPD__STR(x)             EPD__STR2(x)
#define EPD_PANEL_TAG           "HEMA-PANEL-" EPD__STR(EPD_WIDTH) "x" \
                                EPD__STR(EPD_HEIGHT)

/* And the type number itself, which is what a person actually says out loud
 * and what tools/flash.sh takes. It comes from config/tag_types.h, force-
 * included ahead of this header in a firmware build; the fallback is for the
 * host test builds, which compile this driver's neighbours against stubs and
 * never see that header. "0" is not a tag type, so an image built outside the
 * normal path is stamped unusable rather than stamped wrong. */
#if !defined(HEMA_TAG_TYPE_TAG)
#define HEMA_TAG_TYPE_TAG       "HEMA-TAG-TYPE-0"
#endif

/* Likewise the waveform. config/tag_types.h normally defines this from
 * EPD_INIT_FROM_OTP; the fallback covers a build that never saw that header
 * and derives it from whatever the resolution above settled on. */
#if !defined(HEMA_WAVEFORM_TAG)
    #if EPD_INIT_FROM_OTP
        #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-OTP"
    #else
        #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-WAVESHARE"
    #endif
#endif

/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/** Configure DC/RST/BUSY GPIOs. Call once from set_pad_functions(). */
void epd_gpio_init(void);

/** Take the SPI bus back for the panel: restore D/C as a GPIO output and
 *  re-init the SPI master with the panel's settings. Call after anything else
 *  has driven the bus - the boot flash shares CLK/MOSI and claims P0_5, which
 *  is the panel's D/C line. */
void epd_spi_claim(void);

/** Bring the panel out of reset and run the SSD1680 init sequence.
 *  full_lut: true = full-refresh waveform, false = partial-refresh waveform
 *  (mirrors the two 30-byte LUT tables found in the reference firmware). */
void epd_init(bool full_lut);

#if EPD_BITBANG && EPD_PANEL_PROBE
/** True if a panel answers cmd 0x2F (Read Status Bit) with a driven level.
 *
 *  Diagnostic, not a gate: nothing refuses to run because this is false. It
 *  exists because a disconnected panel is otherwise indistinguishable from a
 *  bad init sequence - BUSY reads idle, the refresh returns immediately and
 *  the screen simply stays as it was.
 *
 *  Only for the bit-banged boards. Variant B's D/C pin is the boot flash's
 *  MISO, so turning the bus around there needs care this does not take.
 *
 *  epd_probe_pullup/epd_probe_pulldown hold the two raw reads for a debugger:
 *  equal means a panel drove the line, 0xFF/0x00 means nothing did. */
bool epd_panel_present(void);
extern volatile uint8_t epd_probe_pullup;
extern volatile uint8_t epd_probe_pulldown;
#endif

#if EPD_TEMP_READ
/** The controller's own temperature, in whole degrees Celsius, signed.
 *
 *  Read straight out of the temperature register (cmd 0x1B) after the
 *  controller has sampled its internal sensor. Both the width and the units
 *  are the vendor's: its driver reads exactly one byte here and compares it
 *  against 10 as a signed value, so this is degrees, not sixteenths, and the
 *  fractional low byte is left unread.
 *
 *  EPD_TEMP_UNREAD until epd_init() has run once. Diagnostic for now - read
 *  it over SWD next to s_poll_count. Nothing acts on it.
 */
#define EPD_TEMP_UNREAD  ((int8_t)-128)
extern volatile int8_t epd_temp_c;

/** Sample the sensor and read the temperature register back.
 *
 *  Called at the end of epd_init() on the OTP path, where the load has just
 *  happened anyway. Do not call it on the Waveshare path: loading the
 *  temperature also reloads the OTP waveform, which would overwrite the
 *  hand-written LUT that path just sent. */
int8_t epd_read_temperature(void);
#endif

#if EPD_RESAMPLE_PER_REFRESH
/** Re-sample the sensor and reload the waveform to match.
 *
 *  Call before each refresh. Without it the waveform is whatever the
 *  temperature was when the tag booted, frozen for the life of the boot -
 *  epd_display_start() sends 0x22 = 0xC7, which displays without reloading
 *  either. A tag that boots warm and is then put somewhere cold would keep
 *  using the short warm waveform and under-drive every pixel, which shows up
 *  as ghosting rather than as an error. The panel's OTP table spans 2.6x
 *  between its cold and warm plateaus, so this is not a small effect.
 *
 *  On the Waveshare path the same load pulls the OTP waveform back over the
 *  hand-written LUT, so that build writes its own LUT back afterwards - see
 *  the implementation. Nothing about it is OTP-only.
 *
 *  Costs one command pair and a BUSY wait against a refresh of seconds. Also
 *  refreshes epd_temp_c when EPD_TEMP_READ is on, which is what makes a
 *  displayed temperature current rather than a boot-time souvenir. */
void epd_resample_temperature(void);
#endif

#if EPD_TEMP_SWEEP
/** Number of steps in the sweep, and the range it covers. */
#define EPD_SWEEP_N      16
#define EPD_SWEEP_FIRST  (-20)   /* degrees C of step 0 */
#define EPD_SWEEP_STEP   5

/** Results, filled in by epd_temp_sweep() and read out over SWD.
 *
 *  epd_sweep_asked  what we wrote into the temperature register
 *  epd_sweep_echo   what cmd 0x1B read back afterwards. MUST track `asked`,
 *                   or the forcing did not work and the durations mean
 *                   nothing - check this column first, before reading
 *                   anything into the timings.
 *  epd_sweep_ms     how long that refresh took, in ms. A step here is a band
 *                   boundary in the panel's OTP.
 *  epd_sweep_done   0 until the whole sweep has finished.
 */
extern volatile int8_t   epd_sweep_asked[EPD_SWEEP_N];
extern volatile int8_t   epd_sweep_echo[EPD_SWEEP_N];
extern volatile uint16_t epd_sweep_ms[EPD_SWEEP_N];
extern volatile uint8_t  epd_sweep_done;

/** Run the sweep. Blocks for the whole thing - about a minute - and leaves
 *  the panel showing whatever the last step drew. Bench use only. */
void epd_temp_sweep(void);
#endif

/** Push a full 1bpp framebuffer (EPD_BUF_SIZE bytes, MSB-first per row,
 *  1 = white / 0 = black - matches the vendor's own canvas2bytes() packing,
 *  see PROTOCOL_NOTES.md section 6) into the panel's RAM and start a
 *  full-display refresh.
 *
 *  Returns as soon as the refresh is triggered - the panel then takes ~2 s on
 *  its own. Poll epd_display_busy() for completion; do NOT spin on it.
 *
 *  This is deliberately not a blocking call. The refresh outlasts the BLE
 *  supervision timeout, so blocking through one drops any open connection: a
 *  client could never stay connected for more than a minute (the clock's own
 *  minute tick refreshes), which breaks SUOTA and any multi-second transfer.
 *  Yielding between polls keeps the stack scheduled and the link alive. */
void epd_display_start(const uint8_t *framebuffer);

/** True while the panel is still refreshing. Poll from a timer, not a loop. */
bool epd_display_busy(void);

/** Put the panel into deep sleep to save power between refreshes. */
void epd_sleep(void);

#endif // _EPD_SSD1680_H_
