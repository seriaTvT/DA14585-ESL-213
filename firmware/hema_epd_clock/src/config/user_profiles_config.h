/**
 ****************************************************************************************
 *
 * @file user_profiles_config.h
 *
 * @brief Configuration file for the profiles used in the application.
 *
 * Copyright (C) 2015-2023 Renesas Electronics Corporation and/or its affiliates.
 * All rights reserved. Confidential Information.
 *
 * This software ("Software") is supplied by Renesas Electronics Corporation and/or its
 * affiliates ("Renesas"). Renesas grants you a personal, non-exclusive, non-transferable,
 * revocable, non-sub-licensable right and license to use the Software, solely if used in
 * or together with Renesas products. You may make copies of this Software, provided this
 * copyright notice and disclaimer ("Notice") is included in all such copies. Renesas
 * reserves the right to change or discontinue the Software at any time without notice.
 *
 * THE SOFTWARE IS PROVIDED "AS IS". RENESAS DISCLAIMS ALL WARRANTIES OF ANY KIND,
 * WHETHER EXPRESS, IMPLIED, OR STATUTORY, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. TO THE
 * MAXIMUM EXTENT PERMITTED UNDER LAW, IN NO EVENT SHALL RENESAS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE, EVEN IF RENESAS HAS BEEN ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGES. USE OF THIS SOFTWARE MAY BE SUBJECT TO TERMS AND CONDITIONS CONTAINED IN
 * AN ADDITIONAL AGREEMENT BETWEEN YOU AND RENESAS. IN CASE OF CONFLICT BETWEEN THE TERMS
 * OF THIS NOTICE AND ANY SUCH ADDITIONAL LICENSE AGREEMENT, THE TERMS OF THE AGREEMENT
 * SHALL TAKE PRECEDENCE. BY CONTINUING TO USE THIS SOFTWARE, YOU AGREE TO THE TERMS OF
 * THIS NOTICE.IF YOU DO NOT AGREE TO THESE TERMS, YOU ARE NOT PERMITTED TO USE THIS
 * SOFTWARE.
 *
 ****************************************************************************************
 */

#ifndef _USER_PROFILES_CONFIG_H_
#define _USER_PROFILES_CONFIG_H_

/**
 ****************************************************************************************
 * @defgroup APP_CONFIG
 * @ingroup APP
 * @brief  Application configuration file
 *
 * This file contains the configuration of the profiles used by the application.
 *
 * @{
 ****************************************************************************************
 */

/*
 * DEFINITIONS
 ****************************************************************************************
 */

/***************************************************************************************/
/* Used BLE profiles (used by "rwprf_config.h").                                       */
/***************************************************************************************/

#define CFG_PRF_DISS
#define CFG_PRF_CUST1

/* SUOTA - firmware update over BLE. ON by default; tools/build.sh --no-suota
 * leaves it out.
 *
 * This is the switch that puts the service in the GATT database; rwprf_config.h
 * turns CFG_PRF_SUOTAR into BLE_SUOTA_RECEIVER, which is what the rest of the
 * SDK and our on_suotar_status_change() are guarded on. The receiver module
 * itself has always been in the build - EXCLUDE_DLG_SUOTAR is 0 in
 * user_modules_config.h - so this is the only thing that was missing, and the
 * image we already build for the SWD path is the image SUOTA expects. See
 * hema-local/docs/SUOTA_PLAN.md.
 *
 * Behind a build flag at all - rather than unconditional - because it puts a
 * writable path to the boot flash on the air, and anyone in BLE range can use it.
 * That is a real consideration for a shelf label, though a modest one next to
 * what this firmware already accepts unauthenticated over the same radio: any
 * peer can already replace the displayed face or push an arbitrary image.
 *
 * It defaults ON because the alternative default is worse. An image without the
 * service can only be replaced by attaching SWD to that tag, so choosing it by
 * omission means physically revisiting every tag it was flashed to - and the
 * whole reason this exists is one J-Link and many tags. Two things about the
 * arrangement are worth stating:
 *
 *  - the bank a transfer targets is provably recoverable. A failed transfer
 *    leaves it invalid and the tag boots the other one - measured on this
 *    hardware, three ways, in hema-local/re/type4/suota/README.md.
 *  - a wrong-type image is refused by the *client*, not by the tag. The tag
 *    publishes its identity (HEMA_COMPAT_STR, on Firmware Revision below) and
 *    hema-local/tools/suota.py compares it against the image before sending
 *    anything. A generic SUOTA app would not, and a Type 3 image on a Type 4
 *    tag boots, advertises and leaves only the panel dead. Enforcing it on the
 *    tag needs a patch to the SDK's app_read_image_headers(), which is why it
 *    is the client's job for now.
 *
 * The flag is also how an image that can be updated over the air is told apart
 * from one that cannot - the name on disk says so.
 */
#if defined(EPD_SUOTA) && (EPD_SUOTA)
#define CFG_PRF_SUOTAR
#endif

/***************************************************************************************/
/* Profile application configuration section                                           */
/***************************************************************************************/

/*
 ****************************************************************************************
 * DISS application profile configuration
 ****************************************************************************************
 */

#define APP_DIS_FEATURES                (DIS_MANUFACTURER_NAME_CHAR_SUP | \
                                        DIS_MODEL_NB_STR_CHAR_SUP | \
                                        DIS_SYSTEM_ID_CHAR_SUP | \
                                        DIS_SW_REV_STR_CHAR_SUP | \
                                        DIS_FIRM_REV_STR_CHAR_SUP | \
                                        DIS_PNP_ID_CHAR_SUP)

/// Manufacturer Name (up to 18 chars)
#define APP_DIS_MANUFACTURER_NAME       ("Renesas")
#define APP_DIS_MANUFACTURER_NAME_LEN   (sizeof(APP_DIS_MANUFACTURER_NAME) - 1)

/// Model Number String
#if defined (__DA14586__)
    #define APP_DIS_MODEL_NB_STR            ("DA14586")
#elif defined (__DA14535__)
    #define APP_DIS_MODEL_NB_STR            ("DA14535")
#elif defined (__DA14531_01__)
    #define APP_DIS_MODEL_NB_STR            ("DA14531-01")
#elif defined (__DA14531__)
    #define APP_DIS_MODEL_NB_STR            ("DA14531")
#else
    #define APP_DIS_MODEL_NB_STR            ("DA14585")
#endif
#ifdef __DA14531_01__
#define APP_DIS_MODEL_NB_STR_LEN        (10)
#else
#define APP_DIS_MODEL_NB_STR_LEN        (7)
#endif

/// System ID - LSB -> MSB
#define APP_DIS_SYSTEM_ID               ("\x12\x34\x56\xFF\xFE\x9A\xBC\xDE")
#define APP_DIS_SYSTEM_ID_LEN           (8)

#define APP_DIS_SW_REV                  SDK_VERSION
#define APP_DIS_FIRM_REV                SDK_VERSION

/// Serial Number
#define APP_DIS_SERIAL_NB_STR           ("1.0.0.0-LE")
#define APP_DIS_SERIAL_NB_STR_LEN       (10)

/// Hardware Revision String
#if defined (__DA14586__)
    #define APP_DIS_HARD_REV_STR            ("DA14586")
#elif defined (__DA14535__)
    #define APP_DIS_HARD_REV_STR            ("DA14535")
#elif defined (__DA14531_01__)
    #define APP_DIS_HARD_REV_STR            ("DA14531-01")
#elif defined (__DA14531__)
    #define APP_DIS_HARD_REV_STR            ("DA14531")
#else
    #define APP_DIS_HARD_REV_STR            ("DA14585")
#endif
#ifdef __DA14531_01__
#define APP_DIS_HARD_REV_STR_LEN        (10)
#else
#define APP_DIS_HARD_REV_STR_LEN        (7)
#endif

/// Firmware Revision - our SUOTA compatibility identity, not the SDK version.
///
/// This is the one thing a client needs to know about a tag before pushing
/// firmware to it, and DIS is the standard place to publish it: the client
/// reads it here, compares it with the identity in the image it is about to
/// send, and refuses a mismatch before transferring 40 KB. See HEMA_COMPAT_STR
/// in config/tag_types.h for what the fifteen characters mean and why each of
/// them can cost a panel.
///
/// Nothing is lost by displacing SDK_VERSION: it is still on Software Revision
/// below, and it was never the more useful of the two here since every tag we
/// build reports the same SDK.
#define APP_DIS_FIRM_REV_STR            HEMA_COMPAT_STR
#define APP_DIS_FIRM_REV_STR_LEN        (sizeof(APP_DIS_FIRM_REV_STR) - 1)

/// Software Revision String
#define APP_DIS_SW_REV_STR              SDK_VERSION
#define APP_DIS_SW_REV_STR_LEN          (sizeof(APP_DIS_SW_REV_STR) - 1)

/// IEEE
#define APP_DIS_IEEE                    ("\xFF\xEE\xDD\xCC\xBB\xAA")
#define APP_DIS_IEEE_LEN                (6)

/**
 * PNP ID Value - LSB -> MSB
 *      Vendor ID Source : 0x02 (USB Implementers Forum assigned Vendor ID value)
 *      Vendor ID : 0x045E      (Microsoft Corp)
 *      Product ID : 0x0040
 *      Product Version : 0x0300
 * e.g. #define APP_DIS_PNP_ID          ("\x02\x5E\x04\x40\x00\x00\x03")
 */
#define APP_DIS_PNP_ID                  ("\x01\xD2\x00\x80\x05\x00\x01")
#define APP_DIS_PNP_ID_LEN              (7)

/// @} APP_CONFIG

#endif // _USER_PROFILES_CONFIG_H_
