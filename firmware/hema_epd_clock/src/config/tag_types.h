/**
 ****************************************************************************************
 *
 * @file tag_types.h
 *
 * @brief What still varies between builds. Which tag this runs on is not it.
 *
 * This header used to select a tag: HEMA_TAG_TYPE picked a board variant and a
 * panel size, and every image was built for one of four tags. None of that is
 * true any more. The wiring, the panel geometry and even the default clock face
 * come from the record at flash 0x039000, read at boot - see epd/epd_board.h -
 * so ONE image drives every tag, and there is no type to state.
 *
 * That was proven rather than assumed: an image built for a variant-A 104x212
 * tag drove a variant-B 122x250 panel, with the right pins, 4000-byte frames
 * and the large default face. hema-local/docs/TAG_VARIANTS.md has the whole
 * derivation.
 *
 * What remains here is the ONE axis a tag cannot answer for itself: which
 * waveform its panel wants. That is keyed to the panel LOT, not the board, and
 * nothing in the record distinguishes the lots - two Type 4s disagree, and so
 * do two Type 3s. Getting it wrong leaves the matrix inert with the border
 * flickering, which reads as a broken screen rather than a wrong build.
 *
 ****************************************************************************************
 */

#ifndef _TAG_TYPES_H_
#define _TAG_TYPES_H_

/* Which waveform the panel gets, and why the safe one is the default.
 *
 * Two exist. The Waveshare table we carry is about 2.5x faster; the OTP one is
 * the panel's own, loaded by the controller from its OTP. Neither drives
 * everything:
 *
 * Keyed by the PANEL's lot code, because that is what the requirement tracks -
 * the type number never did. Lot codes are on the panel itself; the
 * full inventory with both silkscreens is hema-local/docs/Screens.txt, and the
 * analysis is hema-local/docs/PANEL_LOTS.md.
 *
 *   lot code         type  Waveshare        OTP
 *   E213A55N18AH28   1     works, 7 steps   untried
 *   E213A41N192QB4   4     works, 7 steps   works, 3003 ms at >=30 C
 *   E213A41N19AS02   3     works, 7 steps   unmeasured
 *   E213A41N195B82   3     works, 10 steps  works, ~3350 ms      [1]
 *   E213A41N194NM1   4     works, 10 steps  works, 3642 ms at >=30 C
 *   E213A55N18CP31   2     - panel damaged, nothing established -
 *
 * The right-hand column of that table is the interesting one: N194NM1 was listed
 * as OTP-only for a month. Its controller runs TEN steps, not seven, so a table
 * written for seven left every phase at zero frames - measured, not guessed, with
 * tools/build.sh --lut-probe. At ten steps it drives the hand-written waveform
 * correctly, 2.35x faster than its own OTP. N195B82 turned out to be the same
 * story and the same fix; see EPD_LUT_STEPS.
 *
 * [1] This panel was destroyed by electrostatic discharge on 2026-08-12, after
 *     the measurements above and unrelated to the firmware - its PCB is fine.
 *     The row stands as a record; the panel is no longer available to re-test,
 *     so treat it as closed rather than confirmable.
 *
 * **Two Type 4s disagree, and so do two Type 3s.** The type number identifies
 * the board, not the panel lot, so no per-type default can be right for every
 * tag of that type - and the one that guesses wrong fails silently, with the
 * matrix dead and only the border moving, which reads as a broken screen rather
 * than a wrong build. That has already cost an evening on the N194NM1 tag.
 *
 * **The default is the Waveshare table**, and OTP is the fallback:
 * `tools/build.sh --otp`. That is a deliberate reversal, decided
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
 *   - flashing the default first and looking at the glass is a five-second
 *     check, not a debugging session;
 *   - and the speed difference is large - roughly 2.5x on a full refresh, and it
 *     is the difference between a partial refresh being worth having and not.
 *
 * So: flash the default, look at the screen, and reach for `--otp` if the matrix
 * stayed still. `tools/build.sh --all` builds both waveforms and both LUT shapes, so the
 * fallback is already on disk when you need it.
 *
 * Do not read a per-type default as a claim about a panel. Two Type 4s and two
 * Type 3s each disagree with each other; the default is a starting guess that is
 * right about half the time on A41 and so far always right on A53.
 */
#if !defined(EPD_INIT_FROM_OTP)
    #define EPD_INIT_FROM_OTP  0
#endif

/* Stamped into the image so the flasher can say which waveform is about to go
 * on and a slow tag is never a mystery. It is the only build axis left. */
#if EPD_INIT_FROM_OTP
    #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-OTP"
#else
    #define HEMA_WAVEFORM_TAG  "HEMA-WAVEFORM-WAVESHARE"
#endif

#define HEMA__STR2(x)       #x
#define HEMA__STR(x)        HEMA__STR2(x)

/* --- the SUOTA compatibility identity -------------------------------------
 *
 * What an over-the-air update could still get wrong, in fifteen characters,
 * because such an update has no operator and no flasher to check it.
 *
 *      U1-W10
 *      ^  ^ ^-- LUT steps: 7 or 10; absent on the OTP waveform
 *      |  +---- waveform: W Waveshare, O the panel's own OTP
 *      +------- the universal generation: pins and geometry come off the tag
 *
 * IT USED TO SAY MORE, and the reason it says less is the point of this whole
 * effort. The old form was T4B-104x212-W10: tag type, board variant, panel
 * geometry, waveform. Each named a way to kill a panel from a distance, and
 * three of them no longer can - the pin map and the geometry are read from the
 * record at flash 0x039000 at boot, so an image that meets the "wrong" tag
 * adapts instead of failing. Verified on hardware: an image built for a
 * variant-A 104x212 tag drove a variant-B 122x250 panel, correct pins, 4000
 * byte frames, and picked the large default face.
 *
 * Leaving those fields in would have been worse than useless. They would have
 * refused updates that are perfectly safe - which is the failure that pushes
 * people to disable the check - while describing a build in terms it no longer
 * has any opinion about.
 *
 * The waveform stays because it is the one axis the board record cannot answer.
 * It is keyed to the PANEL LOT, not the board: two Type 4s disagree and so do
 * two Type 3s (see the table above), and nothing in the record distinguishes
 * E213A41N192QB4 from E213A41N194NM1. A 7-step table on the 10-step controller
 * runs zero frames and leaves the glass blank, which is exactly the invisible
 * bricking this field exists to prevent.
 *
 * NOTE FOR CLIENTS: the geometry moved rather than vanished. It is bytes 10-13
 * of the render-status characteristic (epd_cmd_status(), report format 3),
 * where it is the tag's real panel rather than a build-time claim. A client
 * streaming a raw framebuffer to the image service must read it from there.
 *
 * tools/flash.sh still refuses a mismatch over SWD by reading the stamps out of
 * the binary; those stamps keep describing the build, which is the right thing
 * for a local flash to check. This is the same refusal in the one field the
 * SUOTA image header has spare: version[16], IMAGE_HEADER_VERSION_SIZE.
 *
 * EPD_PARTIAL is deliberately not in it. It changes how the panel is driven,
 * not whether it can be, so a partial build is a legitimate update for a tag
 * running a full-refresh one rather than a mismatch.
 *
 * It lives here, and not beside the other stamps in epd/epd_ssd1680.h, because
 * config/user_profiles_config.h needs it for the Device Information Service and
 * is processed long before that header can be included.
 *
 * Fifteen characters leaves plenty spare now. Keep one spare at least: the SDK
 * memcmp's the field at a fixed sixteen and a silently truncated identity is
 * worse than none, since two different tags would then look alike. */

/* The step count the identity reports, taken from the one place it is defined
 * rather than restated here.
 *
 * This used to hardcode its own `7` to match the driver header's default, with
 * a compile-time assert over there checking the two had not drifted. The assert
 * was dead - see the note where it used to live in epd_ssd1680.h - so the
 * duplication is gone instead: epd_lut_steps.h defines EPD_LUT_STEPS once, both
 * files include it, and there is nothing left to keep in step.
 *
 * Only meaningful on the Waveshare path - the OTP waveform lives in the panel
 * and has no step count of ours. */
#include "epd_lut_steps.h"
#define HEMA_COMPAT_STEPS   EPD_LUT_STEPS

#if EPD_INIT_FROM_OTP
    #define HEMA_COMPAT_WAVE    "O"
#else
    #define HEMA_COMPAT_WAVE    "W" HEMA__STR(HEMA_COMPAT_STEPS)
#endif

/* The generation. Bumped when a tag and an image can disagree about something
 * the identity no longer states - which is precisely what happened here, so
 * "U1" also means "reads its pins and geometry from the board record". An
 * older tag running a T#x-WxH-... image will not match this and will refuse
 * the update, which is correct: it cannot be told apart from a genuinely
 * incompatible one, and the safe answer to that is a flash over SWD. */
#define HEMA_COMPAT_GEN     "U1"

#define HEMA_COMPAT_STR     HEMA_COMPAT_GEN "-" HEMA_COMPAT_WAVE

/* The same string behind a prefix, so tools/mksuota.py can find it in the .bin
 * the way tools/flash.sh finds the other stamps. The bare form is what goes on
 * the air in the Device Information Service, where a client reads it to learn
 * what a tag will accept *before* spending a transfer finding out. */
#define HEMA_COMPAT_TAG     "HEMA-COMPAT-" HEMA_COMPAT_STR

#endif /* _TAG_TYPES_H_ */
