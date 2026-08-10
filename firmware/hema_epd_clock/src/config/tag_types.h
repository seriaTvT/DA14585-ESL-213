/**
 ****************************************************************************************
 *
 * @file tag_types.h
 *
 * @brief Which physical tag this image is built for.
 *
 * One number picks a tag; the board wiring and the panel size follow from the
 * table below. Set it from the build rather than by editing anything:
 *
 *     tools/build.sh --type 3        one image
 *     tools/build.sh --all           one image per type, into out/
 *
 * both of which pass -DHEMA_TAG_TYPE=n. The default here is only what a bare
 * `make all` falls back to.
 *
 * The two axes are independent - board wiring and panel size do not move
 * together, and all four combinations exist in the field - so each type states
 * both rather than deriving one from the other. Which tag is which, and how
 * each was established, is in hema-local/docs/TAG_VARIANTS.md.
 *
 * Identifying a tag you have not seen before: read the panel label off the
 * flex (A53 -> 122x250, A41 -> 104x212), and read the wiring out of the stock
 * firmware's own runtime pin table - eight distinct (port, pin) pairs, at
 * 0x07FD4310 on Types 3 and 4. Do NOT try to tell the variants apart by
 * sampling GPIO modes while the stock firmware boots: the bit-banged pins are
 * outputs only during a transfer, e-paper is bistable so a tag need not
 * refresh at boot at all, and the pins that are driven early (P0_7, P2_1,
 * P2_3) belong to both maps. It reads as variant B and it is wrong.
 *
 * Nothing else in the firmware should test HEMA_TAG_TYPE. Code that cares
 * about the wiring tests EPD_BOARD_VARIANT_A/B and code that cares about the
 * geometry tests EPD_PANEL_LOW_RES, exactly as before - this header only
 * decides which of those are set. A fifth tag that pairs an existing board
 * with an existing panel is then a row here and nothing else.
 *
 ****************************************************************************************
 */

#ifndef _TAG_TYPES_H_
#define _TAG_TYPES_H_

/* The tag this image targets. Overridden from the command line by the build
 * scripts; edit this only if you want a different default for a plain make. */
#if !defined(HEMA_TAG_TYPE)
#define HEMA_TAG_TYPE   4
#endif

/*
 * type | board     | panel        | status
 * -----+-----------+--------------+---------------------------------------
 *   1  | variant B | A53 122x250  | driven - the reference board
 *   2  | variant A | A53 122x250  | UNVERIFIED - the only tag we have with
 *      |           |              | this pairing has a panel that answers on
 *      |           |              | no line, so this build has never been
 *      |           |              | seen to work. It is the right build on
 *      |           |              | paper; that is all anyone can say.
 *   3  | variant A | A41 104x212  | driven
 *   4  | variant B | A41 104x212  | driven
 */
#if   HEMA_TAG_TYPE == 1
    #define EPD_BOARD_VARIANT_B
    #define HEMA_TAG_OTP_DEFAULT 0
#elif HEMA_TAG_TYPE == 2
    #define EPD_BOARD_VARIANT_A
    #define HEMA_TAG_OTP_DEFAULT 0
#elif HEMA_TAG_TYPE == 3
    #define EPD_BOARD_VARIANT_A
    #define EPD_PANEL_LOW_RES
    #define HEMA_TAG_OTP_DEFAULT 0
#elif HEMA_TAG_TYPE == 4
    #define EPD_BOARD_VARIANT_B
    #define EPD_PANEL_LOW_RES
    #define HEMA_TAG_OTP_DEFAULT 0
#else
    #error "HEMA_TAG_TYPE must be 1, 2, 3 or 4 - see hema-local/docs/TAG_VARIANTS.md"
#endif

/* Which waveform the panel gets, and why the safe one is the default.
 *
 * Two exist. The Waveshare table we carry is about 2.5x faster; the OTP one is
 * the panel's own, loaded by the controller from its OTP. Neither drives
 * everything:
 *
 * Keyed by the PANEL's lot code, because that is what the requirement tracks -
 * the type number does not (see below). Lot codes are on the panel itself; the
 * full inventory with both silkscreens is hema-local/docs/Screens.txt, and the
 * analysis is hema-local/docs/PANEL_LOTS.md.
 *
 *   lot code         type  Waveshare        OTP
 *   E213A55N18AH28   1     works, 7 steps   untried
 *   E213A41N192QB4   4     works, 7 steps   works, 3003 ms at >=30 C
 *   E213A41N19AS02   3     works, 7 steps   unmeasured
 *   E213A41N195B82   3     inert at 7 [1]   works, ~3350 ms
 *   E213A41N194NM1   4     works, 10 steps  works, 3642 ms at >=30 C
 *   E213A55N18CP31   2     - panel damaged, nothing established -
 *
 * The right-hand column of that table is the interesting one: N194NM1 was listed
 * as OTP-only for a month. Its controller runs TEN steps, not seven, so a table
 * written for seven left every phase at zero frames - measured, not guessed, with
 * tools/build.sh --lut-probe. At ten steps it drives the hand-written waveform
 * correctly, 2.35x faster than its own OTP. N195B82 fails the same way and is the
 * obvious next one to try at 10; see EPD_LUT_STEPS.
 *
 * [1] Recorded as "hangs" rather than the inert matrix the N194NM1 shows, which
 *     would be a second and different failure mode. The observation was made
 *     before the two Type 3 panels were told apart, so which of them hung is
 *     inference from its being the one that does not take the table. Worth
 *     re-confirming on the panel rather than trusting this line.
 *
 * **Two Type 4s disagree, and so do two Type 3s.** The type number identifies
 * the board, not the panel lot, so no per-type default can be right for every
 * tag of that type - and the one that guesses wrong fails silently, with the
 * matrix dead and only the border moving, which reads as a broken screen rather
 * than a wrong build. That has already cost an evening on the N194NM1 tag.
 *
 * **Every type defaults to the Waveshare table**, and OTP is the fallback:
 * `tools/build.sh --type 4 --otp`. That is a deliberate reversal, decided
 * 2026-08-09, and the reasoning is worth keeping because the opposite default
 * was argued for here for a month.
 *
 * The old default was the waveform that drives every unit of a type we had
 * tested, on the grounds that being slower is recoverable by rebuilding while
 * being invisibly dead costs an evening. What changed is that the failure stopped
 * being invisible:
 *
 *   - the inert failure mode is now understood and instantly recognisable - the
 *     matrix does not move while the border electrode flickers, and
 *     `s_poll_count` reads ~4 instead of ~31 (hema-local/tools/tagread.py);
 *   - the four types are told apart by eye, so flashing the default first and
 *     looking at the glass is a five-second check, not a debugging session;
 *   - and the speed difference is large - roughly 2.5x on a full refresh, and it
 *     is the difference between a partial refresh being worth having and not.
 *
 * So: flash the default, look at the screen, and reach for `--otp` if the matrix
 * stayed still. `tools/build.sh --all` builds both for every type so the
 * fallback is already on disk when you need it.
 *
 * Do not read a per-type default as a claim about a panel. Two Type 4s and two
 * Type 3s each disagree with each other; the default is a starting guess that is
 * right about half the time on A41 and so far always right on A53.
 */
#if !defined(EPD_INIT_FROM_OTP)
    #define EPD_INIT_FROM_OTP  HEMA_TAG_OTP_DEFAULT
#endif

/* Stamped into the image beside the type, so the flasher can say which
 * waveform is about to go on and a slow tag is never a mystery. */
#if EPD_INIT_FROM_OTP
    #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-OTP"
#else
    #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-WAVESHARE"
#endif

/* Stamped into the image so the flasher can check the tag it was told about
 * against the tag the binary was built for. Built by stringifying the number
 * rather than written out per branch, so it cannot drift from the selection
 * above. See EPD_BOARD_VARIANT_TAG in epd_ssd1680.h for why a stamp is worth
 * carrying at all. */
#define HEMA__STR2(x)       #x
#define HEMA__STR(x)        HEMA__STR2(x)
#define HEMA_TAG_TYPE_TAG   "HEMA-TAG-TYPE-" HEMA__STR(HEMA_TAG_TYPE)

/* --- the SUOTA compatibility identity -------------------------------------
 *
 * All of the above condensed into fifteen characters, because a firmware
 * update over the air has no operator and no flasher to check it.
 *
 *      T4B-104x212-W10
 *      ^^ ^  ^      ^ ^-- LUT steps: 7 or 10; absent on the OTP waveform
 *      |  |  |      +---- waveform: W Waveshare, O the panel's own OTP
 *      |  |  +----------- panel geometry
 *      |  +-------------- board variant, which is the wiring
 *      +----------------- tag type
 *
 * Each of those is a way to kill a panel from a distance, and none of them
 * announces itself as a bad update - every one presents as a broken screen.
 * The type and variant decide the pin map; the geometry decides whether the
 * image is merely garbled; the waveform decides whether the matrix moves at
 * all on this panel lot; and the step count decides it again, since a 7-step
 * table on the 10-step controller runs zero frames and leaves the glass blank.
 * tools/flash.sh refuses all of these over SWD by reading the stamps out of the
 * binary. This is the same refusal, in the one field the SUOTA image header has
 * spare: version[16], IMAGE_HEADER_VERSION_SIZE.
 *
 * EPD_PARTIAL is deliberately not in it. It changes how the panel is driven,
 * not whether it can be, so a partial build is a legitimate update for a tag
 * running a full-refresh one rather than a mismatch.
 *
 * It lives here, and not beside the other stamps in epd/epd_ssd1680.h, because
 * config/user_profiles_config.h needs it for the Device Information Service and
 * is processed long before that header can be included. The price is that the
 * geometry and the default step count are restated here rather than derived
 * from EPD_WIDTH/EPD_HEIGHT/EPD_LUT_STEPS - so epd_ssd1680.h asserts at compile
 * time that these agree with those. Change one and the build stops.
 *
 * Fifteen characters leaves one spare. Keep it that way: the SDK memcmp's the
 * field at a fixed sixteen and a silently truncated identity is worse than
 * none, since two different tags would then look alike. */
#if defined(EPD_PANEL_LOW_RES)
    #define HEMA_COMPAT_W       104
    #define HEMA_COMPAT_H       212
#else
    #define HEMA_COMPAT_W       122
    #define HEMA_COMPAT_H       250
#endif

#if defined(EPD_BOARD_VARIANT_A)
    #define HEMA_COMPAT_VARIANT "A"
#else
    #define HEMA_COMPAT_VARIANT "B"
#endif

/* Must match the default in epd_ssd1680.h, which is what the asserted check
 * over there is for. Only meaningful on the Waveshare path - the OTP waveform
 * lives in the panel and has no step count of ours. */
#if !defined(EPD_LUT_STEPS)
    #define HEMA_COMPAT_STEPS   7
#else
    #define HEMA_COMPAT_STEPS   EPD_LUT_STEPS
#endif

#if EPD_INIT_FROM_OTP
    #define HEMA_COMPAT_WAVE    "O"
#else
    #define HEMA_COMPAT_WAVE    "W" HEMA__STR(HEMA_COMPAT_STEPS)
#endif

/* Type and variant together - "T4B". Named because it is used twice: it opens
 * the compatibility identity below, and it is also the readable part of the
 * advertised device name (USER_DEVICE_NAME in user_config.h), so that a scanner
 * showing a list of tags says which kind each one is. Always three characters. */
#define HEMA_COMPAT_TAGID   "T" HEMA__STR(HEMA_TAG_TYPE) HEMA_COMPAT_VARIANT

/* High or low resolution, as one character, for the device name.
 *
 * Strictly redundant - the type number already implies the panel - but the name
 * is read by a person choosing between tags in a scanner, and "is this the
 * 122x250 one?" should not need a lookup table. One character is cheap enough to
 * spend on that. Deliberately NOT in the compatibility identity, which carries
 * the geometry in full because a machine compares it. */
#if defined(EPD_PANEL_LOW_RES)
    #define HEMA_COMPAT_RES     "L"
#else
    #define HEMA_COMPAT_RES     "H"
#endif

#define HEMA_COMPAT_STR     HEMA_COMPAT_TAGID \
                            "-" HEMA__STR(HEMA_COMPAT_W) "x" \
                            HEMA__STR(HEMA_COMPAT_H) "-" HEMA_COMPAT_WAVE

/* The same string behind a prefix, so tools/mksuota.py can find it in the .bin
 * the way tools/flash.sh finds the other stamps. The bare form is what goes on
 * the air in the Device Information Service, where a client reads it to learn
 * what a tag will accept *before* spending a transfer finding out. */
#define HEMA_COMPAT_TAG     "HEMA-COMPAT-" HEMA_COMPAT_STR

#endif /* _TAG_TYPES_H_ */
