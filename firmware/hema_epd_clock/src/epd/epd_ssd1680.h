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
 * Panel geometry. Both sizes are in the field and vary independently of the
 * board wiring, so this comes from the tag type - config/tag_types.h defines
 * EPD_PANEL_LOW_RES for the types that carry the 104x212 A41. High-res is only
 * the fallback for builds that never see that header, the host tests included.
 *
 * Read the size off the FPC label rather than inferring it: A53 is 122x250,
 * A41 and A07 are 104x212.
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
 *   OTP        the sequence the retail firmware uses. Writes no waveform at
 *              all - cmd 0x18 selects the internal temperature sensor and cmd
 *              0x22 bit 4 loads the factory LUT out of the controller's OTP.
 *              Temperature-compensated: measured 2.6x longer at 0 C than at
 *              30 C, so it is also the slower of the two at room temperature.
 *   WAVESHARE  a hand-written LUT via cmd 0x32, from Waveshare's EPD_2IN13_V2
 *              reference. Fixed, temperature-independent, and roughly 2.5x
 *              quicker. 70 bytes at 7 steps, 100 at 10 - see EPD_LUT_STEPS.
 *
 * **The axis is the LUT's SHAPE, not which waveform a panel "accepts".** That
 * reading held for a month and was wrong. A panel whose matrix stayed completely
 * inert on the hand-written table, border flickering, looked like a panel that
 * rejected hand-written waveforms; measured 2026-08-09, its controller simply runs
 * ten steps where Waveshare's table is written for seven, so the table's timing
 * groups landed in the region it reads as voltages and every phase ran zero
 * frames. Rewritten at ten steps, the same panel drives that waveform correctly
 * and 2.35x faster than its OTP one.
 *
 * So there is no panel here that needs OTP - only panels whose step count we had
 * not measured. OTP remains the universal fallback and the only temperature-
 * compensated option, which is worth having on its own merits.
 *
 * Which shape a controller wants is still not predictable from anything visible,
 * and the lot code on the FPC is still the only thing that has tracked it. Flash
 * and look, or read s_poll_count, where a wrong shape is unmistakable: zero frames
 * measures ~230 ms against a working table's ~1.6 s.
 *
 * config/tag_types.h sets the waveform per tag type - every type defaults to
 * WAVESHARE now - and tools/build.sh --otp overrides it per build. The fallback
 * below exists only for builds that never see that header, the host tests.
 *
 * See hema-local/docs/PANEL_LOTS.md, and re/type4/lut/ for the probe data.
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
 * On by default everywhere. It was opt-in on the Waveshare path while
 * sampling meant cmd 0x22 = 0xB1, which drags the OTP waveform in with the
 * temperature and made every refresh rewrite the hand-written LUT to undo it.
 * 0xA1 does not touch the waveform at all (see EPD_TEMP_LOAD_NOLUT), so that
 * reason is gone and the only remaining cost is ~250 bytes and one small SPI
 * transaction per refresh.
 *
 * Leaving it off would mean {T} renders as the literal "{T}" on that build -
 * correct, and a confusing thing to meet on a tag you expected to show a
 * temperature. Set EPD_TEMP_READ to 0 explicitly to get the bytes back.
 */
#if !defined(EPD_TEMP_READ)
    #define EPD_TEMP_READ 1
#endif

/* Whether a refresh re-samples at all. Two independent reasons: an OTP build
 * must, to keep the waveform matched to the current temperature; any build
 * that reports a temperature must, or {T} would show the boot-time value for
 * the life of the boot. */
#define EPD_RESAMPLE_PER_REFRESH  (EPD_INIT_FROM_OTP || EPD_TEMP_READ)

/* Sample the temperature WITHOUT reloading the waveform.
 *
 * cmd 0x22 bit 5 (load temperature) and bit 4 (load LUT) are independent, so
 * 0xA1 samples the sensor and leaves the LUT alone where 0xB1 would replace it
 * with the panel's OTP one. Measured on the tag that drives on either waveform:
 * with 0xA1 the refresh stayed at 31 polls (Waveshare) rather than the ~60 an
 * OTP load would give, and epd_temp_c still read a correct 26 C. Both halves of
 * the claim, one reading.
 *
 * That makes 0xA1 the default, and it is also the safer one: it never touches
 * the waveform, so there is nothing to restore and nothing to restore wrongly.
 * If a controller revision turned out not to honour bit 5 on its own the
 * symptom would be a stale epd_temp_c - visible, and harmless to the display.
 *
 * Set to 0 to go back to 0xB1 followed by rewriting the LUT, which is what the
 * vendor's driver does and is equally proven (31 polls, 27 C on the same tag).
 *
 * Since confirmed on a second panel type and a second board variant - Type 1
 * (variant B, A53) and Type 3 (variant A, A41) both report a sane temperature
 * on it with their refresh unchanged - so the bit semantics do belong to the
 * controller rather than to any one panel, as expected. */
#if !defined(EPD_TEMP_LOAD_NOLUT)
    #define EPD_TEMP_LOAD_NOLUT 1
#endif

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

/* Multiply the Waveshare waveform's drive, to find out WHY some A41 panels sit
 * inert on it while their border electrode moves normally.
 *
 * Two hypotheses produce that exact symptom and the symptom cannot separate
 * them:
 *
 *   under-drive   The table is 60 frames by its own timing groups
 *                 ((3+3)x2 + (9+9)x2 + (3+3)x2) against an OTP refresh of
 *                 3003-3642 ms, so it delivers far less total drive than the
 *                 panel's own waveform. E-paper thresholds are sharp - below
 *                 the voltage-time product the particles do not detach from
 *                 the electrode at all - so "not quite enough" and "nothing"
 *                 look identical. A lot needing 21% more drive (which is what
 *                 the two measured OTP curves differ by) can therefore fall
 *                 off a cliff rather than fade.
 *   wrong shape   The controller wants a different step count, so our 70 bytes
 *                 land misaligned: the timing bytes fall in the voltage region,
 *                 the timing region stays zero, and every phase runs zero
 *                 frames. Not speculation - measured on the Type 5 controller,
 *                 which wants 12 steps and 144 bytes and went inert on this
 *                 same table, BUSY clearing normally included.
 *
 * This scales the repeat count of each timing group, the one knob that changes
 * total drive without touching voltages or the layout. The outcome is binary:
 *
 *   a gain that brings the matrix to life  -> under-drive
 *   nothing at any gain                    -> wrong shape, and the LUT probe
 *                                             ported from Type 5 is next
 *
 * Groups already at zero stay at zero; an unused phase must stay unused. 1
 * ships the table as Waveshare wrote it. 3 puts 180 frames on the panel, the
 * same order as the OTP path's duration, so it is the natural first try.
 *
 * A bench switch, not a shipping default. A gain that works is a finding, not
 * a waveform: multiplying someone else's calibration is not the same as having
 * one, and the panel's own OTP is per-lot correct by construction. */
#if !defined(EPD_LUT_GAIN)
    #define EPD_LUT_GAIN 1
#endif

#if EPD_LUT_GAIN < 1
    #error "EPD_LUT_GAIN is a multiplier - 1 means the table unmodified"
#endif

/* Read whatever identifies the panel, so a build can tell which lot it is on
 * instead of being told.
 *
 * Worth having because the waveform requirement tracks the panel lot and
 * nothing in the firmware can currently see it. Two A41 panels with the same
 * `HINK-E0213A41-FPC` silkscreen and the same PCB need different waveforms, and
 * today the only thing that predicts which is a sticker read by eye. If any of
 * these registers differs between lots, that guessing game is over.
 *
 * Three registers, cheapest confidence first:
 *   0x2F  Status Bit Read. Proven to answer on these tags - epd_panel_present()
 *         already reads it and gets a consistently driven value. May well be
 *         identical across lots, being status rather than identity.
 *   0x2E  Read User ID. SSD16xx parts carry a 10-byte OTP user ID, which is
 *         where a panel maker would put a lot code if it put one anywhere.
 *   0x2D  OTP Register Read for Display Option, 10 bytes. The two panels are
 *         KNOWN to hold different OTP waveform tables - measured across 16
 *         temperature steps, curves that cross - so their OTP differs for
 *         certain. This is the closest we can get to reading that difference.
 *
 * 0x2E and 0x2D are datasheet-level expectations, unverified on this silicon.
 * A read costs nothing and cannot harm the panel, so the risk is a lapful of
 * 0xFF rather than anything worse. Results land in epd_panel_id_* for a
 * debugger; nothing acts on them yet, and nothing should until two panels have
 * actually been compared. */
#if !defined(EPD_PANEL_ID)
    #define EPD_PANEL_ID 0
#endif

/* How many steps the hand-written waveform is written for.
 *
 * 7 is Waveshare's own shape, and the only one that drives the lots which accept
 * their table: voltages `[0:35)` as five groups of seven, timing `[35:70)` as
 * seven groups of five.
 *
 * 10 is the shape MEASURED on the SLH1904 panel with --lut-probe: voltages
 * `[0:50)`, timing `[50:100)` as ten groups of five, and nothing beyond byte 100.
 * That is why the 7-step table leaves those panels inert - its non-zero timing
 * groups land at `[35:50)`, which this controller reads as voltages, so its timing
 * region receives only the zeros our table happens to have at `[50:70)` and every
 * phase runs zero frames.
 *
 * The tables at each shape are the same waveform transposed, not a redesign: the
 * same voltage levels, the same three active phases, the same repeat counts, and
 * the same five register bytes afterwards. Only the geometry differs.
 *
 * A panel takes one shape or the other, and which one is a property of the
 * bonded controller. Flash it and look, exactly as with the waveform choice - or
 * read s_poll_count, which is quicker and less ambiguous: at ~19.8 ms/frame over
 * ~230 ms of overhead, the 60-frame full table should measure ~1420 ms (~28
 * polls). A table of the wrong shape runs zero frames and measures ~230 ms
 * (~4 polls), which is unmistakable and needs no glass. */
#if !defined(EPD_LUT_STEPS)
    #define EPD_LUT_STEPS 7
#endif

#if EPD_LUT_STEPS == 7
    #define EPD_LUT_BYTES   70u     /* payload of cmd 0x32 */
    #define EPD_LUT_TIMING  35u     /* where the timing groups start */
#elif EPD_LUT_STEPS == 10
    #define EPD_LUT_BYTES   100u
    #define EPD_LUT_TIMING  50u
#else
    #error "EPD_LUT_STEPS must be 7 (Waveshare's shape) or 10 (measured on A41)"
#endif

/* Five bytes per timing group, and the fifth is the repeat count. Measured, not
 * assumed: the probe raised the duration for four bytes out of every five. */
#define EPD_LUT_TIMING_GROUP 5u

/* One more settling step than Waveshare's table has room for, weighted toward
 * black.
 *
 * Waveshare's waveform transposed drives correctly but lands black slightly
 * faint, most visibly on single-pixel strokes: a thin feature has neighbours
 * pulling the other way, so it sees a weaker effective field and needs longer to
 * saturate. Ten of the controller's steps exist and only three are used, so there
 * is room to settle black further - which the 7-step table did not have.
 *
 * **The sub-phase structure is what makes this work, and getting it wrong was
 * instructive.** A step's four durations TPA..TPD are shared by every group, but
 * each group's voltage byte says what IT does in each sub-phase. That is how
 * Waveshare's own step 2 serves both directions at once: black settles during
 * sub-phase A (0x40) while white settles during B (0x20). One step, two
 * directions, opposite polarities, no interference.
 *
 * The first attempt ignored that and drove black in A with the white groups left
 * at zero - a unipolar push at black alone. It over-shot badly and the symptom was
 * not "too black": it was black BLEEDING into the pixels around it, like ink off a
 * pen. Driving a pixel hard with nothing driving its neighbour leaves the fringe
 * field to pull that neighbour along, and with no counter-phase to clean it up the
 * spill just stays. More drive was the right idea; unbalanced drive was not.
 *
 * So this step copies step 2's voltage column exactly - black in A, white in B -
 * and only weights the DURATIONS toward black. White is still driven, so the
 * fringe is corrected rather than abandoned, and the extra frames still land where
 * the faintness is.
 *
 * The two dials, in frames:
 *   BLACK  sub-phase A, how much longer black gets to settle
 *   WHITE  sub-phase B, enough to hold the neighbours; do not set it to 0
 *
 * BLACK swept on the SLH1904 panel: 8 still faint, **10 good**, 12 and 14 no
 * darker. So 10 is not a preference, it is where the film saturates - past it the
 * extra frames are spent for nothing, and 14 was as clean as 10, so the ceiling is
 * the pigment rather than the waveform running out of headroom.
 *
 * Set BLACK to 0 to drop the step and get the plain transposition back. At 10 the
 * step is ~198 ms and a full refresh ~1.6 s, against the OTP path's 3.6 s. */
#if !defined(EPD_LUT_BLACK_FRAMES)
    #define EPD_LUT_BLACK_FRAMES 10
#endif
#if !defined(EPD_LUT_WHITE_FRAMES)
    #define EPD_LUT_WHITE_FRAMES 2
#endif

/* The settling levels, taken from step 2 of Waveshare's own table rather than
 * invented: 0x40 settles black in sub-phase A, 0x20 settles white in B. */
#define EPD_LUT_BLACK_LEVEL 0x40u
#define EPD_LUT_WHITE_LEVEL 0x20u

/* The five registers that only exist to go with a hand-written waveform, carried
 * after the payload rather than in it: 0x03 gate voltage, 0x04 source voltage
 * (three bytes), 0x3A dummy line period, 0x3B gate line width. */
#define EPD_LUT_TRAILER 6u

/* Measure the controller's LUT layout, by asking it to time itself.
 *
 * The question this answers: these panels reject the hand-written Waveshare
 * table, running zero frames from it, and the most likely reason is that their
 * LUT is a different shape - a different step count, so our 70 bytes land in the
 * wrong fields. We cannot write any waveform, full or partial, until we know the
 * real layout, and the panels that need one hold no partial waveform in OTP.
 *
 * The method needs no instruments. Write a LUT that is entirely zero except for
 * one marker byte, trigger an update WITHOUT reloading from OTP, and time it.
 * A marker in a phase-duration field makes the update measurably longer; a marker
 * anywhere the controller reads as a voltage does not. Sweep the marker across
 * every offset and the durations draw the register's map: where the timing region
 * starts, how the groups repeat, and where the register ends - because offsets
 * past the end read as baseline.
 *
 * **It cannot drive the panel.** Exactly one byte is non-zero, so either it is a
 * voltage with every duration at zero, or a duration with every voltage at zero.
 * Never both, so no pixel is ever driven and the glass keeps its picture.
 *
 * Proven on the Type 5 controller, where it recovered a 144-byte, 12-step layout
 * from nothing. Worth running on a panel that ACCEPTS the Waveshare table as a
 * control: the timing region should appear at bytes 35-70, which is a positive
 * control on the probe itself rather than only on the panel.
 *
 * A bench build. It blocks for several minutes at boot and never returns, so the
 * BLE stack never starts - read the results over SWD. */
#if !defined(EPD_LUT_PROBE)
    #define EPD_LUT_PROBE 0
#endif

/* ---- partial refresh --------------------------------------------------------
 * Repaint only the rows that changed, with the partial waveform, instead of
 * driving every gate line through the full one.
 *
 * OFF BY DEFAULT and it should stay off until it has been seen on glass. Two
 * pieces of it are read from references rather than measured on this silicon -
 * the 0x22 activation value on each path, and whether this controller wants the
 * previous frame in RAM bank 0x26 - and a partial refresh that half works looks
 * like a working one until the ghosting builds up.
 *
 * What it costs when on: EPD_BUF_SIZE of RAM for the shadow (2756 bytes on A41,
 * 4000 on A53) plus about 40 bytes of state.
 *
 * The shadow is what the panel is believed to be showing. It is only a belief,
 * so anything that could make it wrong invalidates it and forces a full refresh:
 * boot, epd_init() (its SWRESET clears the controller's RAM), and epd_sleep().
 */
#if !defined(EPD_PARTIAL)
    #define EPD_PARTIAL 0
#endif

/* Force a full refresh after this many consecutive partials.
 *
 * Partial waveforms do not fully clear the previous image - that is what makes
 * them fast - so residue accumulates and has to be swept out periodically. The
 * number is a starting guess, not a measurement: panel makers commonly suggest
 * single digits, and the cost of being wrong in the safe direction is one slow
 * refresh in eight. Raise it once ghosting has actually been looked at over a
 * long run. */
#if !defined(EPD_PARTIAL_RUN_MAX)
    #define EPD_PARTIAL_RUN_MAX 8
#endif

/* And force one after this long, however few partials it took.
 *
 * EPD_PARTIAL_RUN_MAX counts partials, which makes it blind to time - and the
 * repaint interval is set by the face, not by us. At EVERY(1) a clock reaches
 * eight partials in eight minutes. A calendar at EVERY(1440) repaints once a day,
 * so it would take **eight days** to earn a full refresh, sitting on accumulated
 * residue the whole time. Same policy, wildly different behaviour, and only
 * because a count is not a duration.
 *
 * An hour bounds it for any face. It also settles the calendar case pleasantly on
 * its own: a face that repaints daily will always find an hour has passed, so it
 * gets a full refresh every time - which is exactly right, because a partial
 * refresh buys nothing when repaints are a day apart, and a clean image is worth
 * everything. The policy ends up matching itself to the face without being told.
 *
 * Panel guidance is usually "at least one full refresh per 24 h". That is an
 * upper bound on neglect, not a target; an hour costs at most one slow refresh an
 * hour and keeps residue short-lived. */
#if !defined(EPD_FULL_MAX_SECS)
    #define EPD_FULL_MAX_SECS 3600u
#endif

/* Above this many dirty rows, refresh fully instead.
 *
 * Not for speed - a partial is cheaper at any size - but for looks: a change
 * covering most of the panel is a new image rather than an update, and a partial
 * waveform would leave the old one faintly underneath it. Three quarters is a
 * guess in the "prefer quality" direction. */
#if !defined(EPD_PARTIAL_MAX_ROWS)
    #define EPD_PARTIAL_MAX_ROWS ((EPD_HEIGHT * 3) / 4)
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

/* HEMA_COMPAT_STR, the SUOTA compatibility identity, is built in
 * config/tag_types.h rather than here - it has to be visible to
 * config/user_profiles_config.h, which is processed long before this header
 * and cannot include it.
 *
 * The cost of living there is that it restates the geometry and the default
 * LUT step count instead of deriving them from EPD_WIDTH/EPD_HEIGHT/
 * EPD_LUT_STEPS. This is where that is checked. A mismatch would be silent and
 * expensive: the identity is what a client uses to decide an image is safe to
 * push, so an identity that disagrees with the build would licence exactly the
 * transfer it exists to prevent.
 *
 * Spelled as a negative array size rather than _Static_assert because this
 * header is also compiled by the host tests at -std=c99.
 *
 * Guarded on HEMA_COMPAT_W for the same reason HEMA_TAG_TYPE_TAG above has a
 * fallback: the host tests compile this header without config/tag_types.h, so
 * there is no identity to check against and an unguarded reference is simply
 * an undefined identifier. A firmware build always has it, which is the build
 * where a mismatch could reach a tag. */
#if defined(HEMA_COMPAT_W)
typedef char epd_compat_geometry_agrees[
    (HEMA_COMPAT_W == EPD_WIDTH && HEMA_COMPAT_H == EPD_HEIGHT) ? 1 : -1];
#if !EPD_INIT_FROM_OTP
typedef char epd_compat_lut_steps_agree[
    (HEMA_COMPAT_STEPS == EPD_LUT_STEPS) ? 1 : -1];
#endif
#endif /* HEMA_COMPAT_W */

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

#if EPD_LUT_PROBE

/** How many bytes of 0x32 payload to sweep. Past any plausible LUT length on
 *  purpose - 70 for the SSD1675A family, 144 measured on Type 5's controller, 153
 *  for the SSD1681 - because offsets the controller ignores read as baseline, and
 *  that is how the register's real length gets measured instead of assumed. */
#define EPD_LUT_PROBE_LEN 160u

/** Update duration in ms for a marker at each offset. Index EPD_LUT_PROBE_LEN
 *  holds the all-zeros control, written FIRST so a sweep that finds nothing can
 *  still be told from a sweep that never ran.
 *
 *  Reading it: entries equal to the control are bytes the controller does not
 *  treat as a duration. A run of raised entries is the timing region, and its
 *  stride is the group size. Everything past the register's end returns to
 *  baseline. */
extern volatile uint16_t epd_lut_probe_ms[EPD_LUT_PROBE_LEN + 1u];
extern volatile uint16_t epd_lut_probe_done;   /* offsets completed so far */

/** Run the sweep. Blocks for minutes and never returns. */
void epd_lut_probe(void);
#endif

#if EPD_PANEL_ID

/** How many bytes to clock out of each identity register. The user ID and the
 *  display-option OTP are documented as 10 bytes; 12 is read so that a register
 *  which is actually longer shows itself rather than being silently truncated,
 *  and so that a register which is shorter shows what it repeats or pads with.
 *  Over-reading a shift register costs nothing but clock cycles. */
#define EPD_PANEL_ID_LEN 12u

/** Filled in by epd_panel_read_id(), read out over SWD.
 *
 *  epd_panel_id_status  cmd 0x2F, one byte.
 *  epd_panel_id_user    cmd 0x2E, the OTP user ID.
 *  epd_panel_id_option  cmd 0x2D, the display-option OTP.
 *  epd_panel_id_done    0 until the read has finished.
 *
 *  Each register is read TWICE - once with no pull, then again with the pad's
 *  internal pull-up - because a register of all zeros is ambiguous and we have
 *  met exactly that case. Two A41 panels returned 0x00 for all three registers
 *  while the temperature read on the same tags worked, which admits two
 *  readings: the controller drove zeros, or the command is unsupported, the
 *  controller drove nothing, and the floating pad read low because we had just
 *  finished clocking a low bit out of it.
 *
 *  The pull-up separates them, with no ambiguity left:
 *
 *    *_pu equals the plain read   the controller drove the line. The value is
 *                                 real data and a zero is a real zero.
 *    *_pu reads 0xFF              nothing drove the line; it followed the pull.
 *                                 The command is not supported here and the
 *                                 plain read means nothing.
 *
 *  Same trick epd_panel_present() uses, and for the same reason - but done on
 *  these three registers, and on both board variants rather than bitbang only. */
extern volatile uint8_t epd_panel_id_status;
extern volatile uint8_t epd_panel_id_user[EPD_PANEL_ID_LEN];
extern volatile uint8_t epd_panel_id_option[EPD_PANEL_ID_LEN];
extern volatile uint8_t epd_panel_id_status_pu;
extern volatile uint8_t epd_panel_id_user_pu[EPD_PANEL_ID_LEN];
extern volatile uint8_t epd_panel_id_option_pu[EPD_PANEL_ID_LEN];
extern volatile uint8_t epd_panel_id_done;

/** Read the identity registers. Non-destructive: it clocks data out and writes
 *  nothing, so it neither refreshes the panel nor disturbs the waveform, and it
 *  is safe to call on a tag that is otherwise working normally. */
void epd_panel_read_id(void);
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
 *  Yielding between polls keeps the stack scheduled and the link alive.
 *
 *  With EPD_PARTIAL on it may instead repaint only the changed rows, or send
 *  nothing at all - hence the return value, which the caller MUST act on.
 *  EPD_PAINT_NONE means no refresh was triggered, so BUSY will never rise and a
 *  caller that armed its poll timer anyway would sit through the whole timeout
 *  before deciding the panel had failed. */
typedef enum {
    EPD_PAINT_NONE = 0,   /* frame identical to the glass; nothing was sent */
    EPD_PAINT_PARTIAL,    /* changed rows only, partial waveform */
    EPD_PAINT_FULL,       /* every row, full waveform */
} epd_paint_t;

epd_paint_t epd_display_start(const uint8_t *framebuffer);

#if EPD_PARTIAL
/** Forget what the panel is believed to hold, forcing the next refresh to be a
 *  full one. Called for you by epd_init() and epd_sleep(); exposed because a
 *  caller that has reason to doubt the glass should be able to say so.
 *
 *  Declared only on the partial path, because that is the only build that has
 *  a belief to discard - there is no shadow without EPD_PARTIAL and every
 *  refresh is already full. The declaration used to be unconditional, which
 *  turned calling it from unguarded code into a link error naming a function
 *  this header appears to promise; guarded, it is a compile error at the call
 *  site instead. */
#if EPD_PARTIAL
void epd_display_forget(void);
#endif

/** Partials since the last full refresh, and what the last paint did. For SWD
 *  and for the render report - a tag that has quietly stopped doing partials is
 *  worth being able to see. */
extern volatile uint8_t epd_partial_run;
extern volatile uint8_t epd_last_paint;   /* epd_paint_t */
#endif

/** True while the panel is still refreshing. Poll from a timer, not a loop. */
bool epd_display_busy(void);

/** Put the panel into deep sleep to save power between refreshes. */
void epd_sleep(void);

#endif // _EPD_SSD1680_H_
