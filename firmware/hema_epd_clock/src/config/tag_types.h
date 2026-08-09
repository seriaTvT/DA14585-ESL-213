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

#endif /* _TAG_TYPES_H_ */
