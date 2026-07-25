/**
 * user_custs1_def.h - Custom Server 1 (CUSTS1) profile database definitions.
 *
 * Defines two GATT services matching the UUIDs discovered by reverse
 * engineering the community reference firmware's companion Web Bluetooth
 * page (webpage/esl_clock.php, see PROTOCOL_NOTES.md section 7). Reusing
 * the same UUIDs means the existing esl_clock.php web tool can talk to
 * this firmware without modification.
 *
 *  - Command service: ASCII drawing-command text, newline terminated
 *    (see epd_cmdparser.h). UUIDs 00001f10 / 00001f1f are 16-bit UUIDs
 *    expanded into the standard Bluetooth Base UUID
 *    (0000XXXX-0000-1000-8000-00805F9B34FB).
 *  - Image service: raw 1bpp framebuffer bytes (see epd_gfx.h), full
 *    128-bit vendor-specific UUIDs.
 *
 * The vendor's third, SUOTA-based OTA service (0000fef5-...) is NOT
 * implemented here - out of scope for this first pass, see
 * PROTOCOL_NOTES.md section 7.
 *
 * UUID byte arrays below are little-endian (as transmitted over the air /
 * as the SDK's other examples store them) - i.e. the reverse of the
 * standard dashed display string.
 */

#ifndef _USER_CUSTS1_DEF_H_
#define _USER_CUSTS1_DEF_H_

#include "attm_db_128.h"

/* ---- Command service: 00001f10-0000-1000-8000-00805f9b34fb
 *      char:            00001f1f-0000-1000-8000-00805f9b34fb ------------- */
#define DEF_CMD_SVC_UUID_128        {0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x10,0x1f,0x00,0x00}
#define DEF_CMD_CHAR_UUID_128       {0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x10,0x00,0x00,0x1f,0x1f,0x00,0x00}

/* Max single BLE write payload we'll accept for one command batch. */
#define DEF_CMD_CHAR_LEN            128
#define DEF_CMD_CHAR_USER_DESC      "Draw Command"

/* ---- Image service: 13187b10-eba9-a3ba-044e-83d3217d9a38
 *      char:            4b646063-6264-f3a7-8941-e65356ea82fe ------------- */
#define DEF_IMG_SVC_UUID_128        {0x38,0x9a,0x7d,0x21,0xd3,0x83,0x4e,0x04,0xba,0xa3,0xa9,0xeb,0x10,0x7b,0x18,0x13}
#define DEF_IMG_CHAR_UUID_128       {0xfe,0x82,0xea,0x56,0x53,0xe6,0x41,0x89,0xa7,0xf3,0x64,0x62,0x63,0x60,0x64,0x4b}

/* Chunk size for a single image-upload write; the full framebuffer
 * (EPD_BUF_SIZE bytes) is assembled across multiple writes - see the
 * handler in user_empty_peripheral_template.c. */
#define DEF_IMG_CHAR_LEN            128
#define DEF_IMG_CHAR_USER_DESC      "Image Data"

/// Custom1 Service Data Base Characteristic enum
enum
{
    CMD_SVC_IDX_SVC = 0,
    CMD_SVC_IDX_CHAR,
    CMD_SVC_IDX_VAL,
    CMD_SVC_IDX_USER_DESC,

    IMG_SVC_IDX_SVC,
    IMG_SVC_IDX_CHAR,
    IMG_SVC_IDX_VAL,
    IMG_SVC_IDX_USER_DESC,

    CUSTS1_IDX_NB
};

#endif // _USER_CUSTS1_DEF_H_
