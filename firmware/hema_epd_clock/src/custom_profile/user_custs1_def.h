/**
 * user_custs1_def.h - Custom Server 1 (CUSTS1) profile database definitions.
 *
 * Two GATT services, both with UUIDs of our own:
 *
 *  - Command service. ASCII drawing-command text, newline terminated, plus a
 *    read-only characteristic reporting what the last render made of it. See
 *    epd_cmdparser.h for the language and the status layout.
 *  - Image service. A raw 1bpp framebuffer, EPD_BUF_SIZE bytes, streamed from
 *    the top with no header and no offset. See epd_gfx.h.
 *
 * These began as the UUIDs the vendor's own web tool used, so that tool could
 * drive this firmware unmodified. That stopped being worth having once the
 * command language diverged: the tool would still connect and still push, and
 * produce a garbage face with nothing reporting a problem. They are ours now,
 * and an old client fails to find the service instead - see the note above
 * each pair.
 *
 * There is no OTA service. The vendor's is SUOTA-based (0000fef5-...) and
 * flashing here goes over SWD; adding it is a decision to be taken on its own
 * merits rather than inherited.
 *
 * UUID byte arrays below are little-endian (as transmitted over the air, and
 * as the SDK's other examples store them) - i.e. the reverse of the standard
 * dashed display string. webui/ble.js carries the same values in text order,
 * and a test compares the two rather than trusting the transcription.
 */

#ifndef _USER_CUSTS1_DEF_H_
#define _USER_CUSTS1_DEF_H_

#include "attm_db_128.h"
#include "epd_cmdparser.h"      /* EPD_STATUS_LEN */

/* ---- Command service: 677fb260-1fc0-42c5-ab6e-e64e0c591714
 *      char:            c0339b97-4239-4aea-a775-988f9c4d2548 -------------
 *
 * Ours, not the vendor's 00001f1x block, and the change is the point rather
 * than tidiness. The language behind these handles is no longer theirs: a
 * client written for the original firmware that still found this service
 * would connect, write, be acknowledged, and paint a garbage face - FONT()
 * ignored, ROTATE(3) refused - with nothing anywhere reporting a problem.
 * Different UUIDs turn that subtly wrong result into an honest "service not
 * found" at the first attempt.
 *
 * Nothing is lost by moving: no service UUID is advertised (see
 * USER_ADVERTISE_DATA), so discovery is by device name and unaffected. */
#define DEF_CMD_SVC_UUID_128        {0x14,0x17,0x59,0x0c,0x4e,0xe6,0x6e,0xab,0xc5,0x42,0xc0,0x1f,0x60,0xb2,0x7f,0x67}
#define DEF_CMD_CHAR_UUID_128       {0x48,0x25,0x4d,0x9c,0x8f,0x98,0x75,0xa7,0xea,0x4a,0x39,0x42,0x97,0x9b,0x33,0xc0}

/* Max single BLE write payload we'll accept for one command batch. */
#define DEF_CMD_CHAR_LEN            128
#define DEF_CMD_CHAR_USER_DESC      "Draw Command"

/* ---- Status characteristic: f2edaa0b-ce5d-4897-ab67-d6f7a3cc453a ---------
 *
 * Read-only. Reports what the last render made of the stored script - see
 * epd_cmd_status() in epd_cmdparser.h for the layout.
 *
 * The one thing the tag could never do was say why a face looked wrong. A
 * skipped line and a line that drew exactly what it was told to draw are the
 * same picture from the outside, so the only way to tell them apart was an
 * SWD dump of the framebuffer - which needs the debugger, the VM and the
 * probe, for what is usually a typo.
 *
 * Served live from a CUSTS1_VALUE_REQ_IND rather than a cached copy pushed
 * into the attribute database, so it cannot report a stale render. The
 * generated UUID is deliberately unrelated to the vendor's 00001f1x block:
 * this characteristic is ours, and the rest follow in the UUID change. */
#define DEF_STATUS_CHAR_UUID_128    {0x3a,0x45,0xcc,0xa3,0xf7,0xd6,0x67,0xab,0x97,0x48,0x5d,0xce,0x0b,0xaa,0xed,0xf2}
#define DEF_STATUS_CHAR_LEN         EPD_STATUS_LEN
#define DEF_STATUS_CHAR_USER_DESC   "Render Status"

/* ---- Image service: 86c08205-f21a-4257-aabd-4602d25c2448
 *      char:            855c0ea3-ae40-4bab-8a7a-52d86e9a5a2b -------------
 *
 * Moved with the command service. The image format itself is unchanged - a
 * raw EPD_BUF_SIZE framebuffer - but a client that can find one service and
 * not the other is a worse failure than one that finds neither. */
#define DEF_IMG_SVC_UUID_128        {0x48,0x24,0x5c,0xd2,0x02,0x46,0xbd,0xaa,0x57,0x42,0x1a,0xf2,0x05,0x82,0xc0,0x86}
#define DEF_IMG_CHAR_UUID_128       {0x2b,0x5a,0x9a,0x6e,0xd8,0x52,0x7a,0x8a,0xab,0x4b,0x40,0xae,0xa3,0x0e,0x5c,0x85}

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

    /* Status lives in the command service rather than one of its own: it
     * reports on what the command characteristic was fed. */
    STATUS_IDX_CHAR,
    STATUS_IDX_VAL,
    STATUS_IDX_USER_DESC,

    IMG_SVC_IDX_SVC,
    IMG_SVC_IDX_CHAR,
    IMG_SVC_IDX_VAL,
    IMG_SVC_IDX_USER_DESC,

    CUSTS1_IDX_NB
};

#endif // _USER_CUSTS1_DEF_H_
