/**
 ****************************************************************************************
 *
 * @file epd_lut_steps.h
 *
 * @brief The LUT step count, defined once.
 *
 * This exists so that two files can agree about EPD_LUT_STEPS without either
 * one restating it.
 *
 * epd/epd_ssd1680.h needs it to size the waveform table. config/tag_types.h
 * needs it to build the SUOTA identity - and cannot include the driver header,
 * because config/user_profiles_config.h consumes that identity long before
 * epd_ssd1680.h can be reached. So tag_types.h used to hardcode the same
 * default as a literal, with a compile-time assert over in the driver header
 * checking the two still matched.
 *
 * That assert had been dead for some time: it was nested inside a guard on
 * HEMA_COMPAT_W, and HEMA_COMPAT_W stopped being defined when the SUOTA
 * identity dropped the panel geometry. Nothing noticed, because both defaults
 * happened to be 7. Had the driver's default moved to 10 on its own, a default
 * build would have compiled a ten-step table and stamped it "U1-W7" - and a
 * genuine seven-step tag would then have accepted that image over the air as
 * compatible and gone blank, with no operator present. That is the exact
 * failure the identity exists to prevent.
 *
 * One definition removes the disagreement rather than detecting it, so there is
 * no longer an invariant here to assert.
 *
 * Kept deliberately small: config/ includes this, so anything else added here
 * would be pulled into the configuration headers too.
 *
 ****************************************************************************************
 */

#ifndef _EPD_LUT_STEPS_H_
#define _EPD_LUT_STEPS_H_

/* Which LUT shape the hand-written Waveshare table is written for.
 *
 * 7 is Waveshare's own shape; 10 was measured on the A41 controller with
 * tools/build.sh --lut-probe. A panel takes one or the other and it is a
 * property of the bonded controller, not of the board or the panel model - see
 * the lot table in config/tag_types.h. Override per build with
 * tools/build.sh --lut-steps <n>.
 *
 * Only meaningful on the Waveshare path: the OTP waveform lives in the panel
 * and has no step count of ours.
 */
#if !defined(EPD_LUT_STEPS)
    #define EPD_LUT_STEPS 7
#endif

#endif /* _EPD_LUT_STEPS_H_ */
