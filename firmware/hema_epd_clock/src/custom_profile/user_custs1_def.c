/**
 * user_custs1_def.c - see user_custs1_def.h for provenance notes.
 */

#include <stdint.h>
#include "co_utils.h"
#include "prf_types.h"
#include "attm_db_128.h"
#include "user_custs1_def.h"

/* ---- Command service ---- */
static const att_svc_desc128_t custs1_cmd_svc = DEF_CMD_SVC_UUID_128;
static const uint8_t CMD_CHAR_UUID_128[ATT_UUID_128_LEN] = DEF_CMD_CHAR_UUID_128;

/* ---- Image service ---- */
static const att_svc_desc128_t custs1_img_svc = DEF_IMG_SVC_UUID_128;
static const uint8_t IMG_CHAR_UUID_128[ATT_UUID_128_LEN] = DEF_IMG_CHAR_UUID_128;

/* Attribute specifications */
static const uint16_t att_decl_svc       = ATT_DECL_PRIMARY_SERVICE;
static const uint16_t att_decl_char      = ATT_DECL_CHARACTERISTIC;
static const uint16_t att_desc_user_desc = ATT_DESC_CHAR_USER_DESCRIPTION;

const uint8_t custs1_services[]  = {CMD_SVC_IDX_SVC, IMG_SVC_IDX_SVC, CUSTS1_IDX_NB};
const uint8_t custs1_services_size = ARRAY_LEN(custs1_services) - 1;
const uint16_t custs1_att_max_nb = CUSTS1_IDX_NB;

/// Full CUSTS1 Database Description - Used to add attributes into the database
const struct attm_desc_128 custs1_att_db[CUSTS1_IDX_NB] =
{
    /*************************
     * Command service
     *************************/

    [CMD_SVC_IDX_SVC]  = {(uint8_t*)&att_decl_svc, ATT_UUID_128_LEN, PERM(RD, ENABLE),
                            sizeof(custs1_cmd_svc), sizeof(custs1_cmd_svc), (uint8_t*)&custs1_cmd_svc},

    [CMD_SVC_IDX_CHAR] = {(uint8_t*)&att_decl_char, ATT_UUID_16_LEN, PERM(RD, ENABLE),
                            0, 0, NULL},

    /* Accept both write-with-response and write-without-response, since we
     * don't yet know which one the vendor's esl_clock.php app uses for its
     * "串口服务" (serial/UART-like) characteristic - see PROTOCOL_NOTES.md
     * section 7. */
    [CMD_SVC_IDX_VAL]  = {CMD_CHAR_UUID_128, ATT_UUID_128_LEN,
                            PERM(WR, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE),
                            DEF_CMD_CHAR_LEN, 0, NULL},

    [CMD_SVC_IDX_USER_DESC] = {(uint8_t*)&att_desc_user_desc, ATT_UUID_16_LEN, PERM(RD, ENABLE),
                            sizeof(DEF_CMD_CHAR_USER_DESC) - 1, sizeof(DEF_CMD_CHAR_USER_DESC) - 1,
                            (uint8_t *) DEF_CMD_CHAR_USER_DESC},

    /*************************
     * Image service
     *************************/

    [IMG_SVC_IDX_SVC]  = {(uint8_t*)&att_decl_svc, ATT_UUID_128_LEN, PERM(RD, ENABLE),
                            sizeof(custs1_img_svc), sizeof(custs1_img_svc), (uint8_t*)&custs1_img_svc},

    [IMG_SVC_IDX_CHAR] = {(uint8_t*)&att_decl_char, ATT_UUID_16_LEN, PERM(RD, ENABLE),
                            0, 0, NULL},

    [IMG_SVC_IDX_VAL]  = {IMG_CHAR_UUID_128, ATT_UUID_128_LEN,
                            PERM(WR, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE),
                            DEF_IMG_CHAR_LEN, 0, NULL},

    [IMG_SVC_IDX_USER_DESC] = {(uint8_t*)&att_desc_user_desc, ATT_UUID_16_LEN, PERM(RD, ENABLE),
                            sizeof(DEF_IMG_CHAR_USER_DESC) - 1, sizeof(DEF_IMG_CHAR_USER_DESC) - 1,
                            (uint8_t *) DEF_IMG_CHAR_USER_DESC},
};
