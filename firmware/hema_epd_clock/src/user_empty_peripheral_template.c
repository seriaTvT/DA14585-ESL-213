/**
 * user_empty_peripheral_template.c - Hema-tag replacement firmware, app glue.
 *
 * Dispatches BLE writes to the command service (see epd_cmdparser.h) and
 * the image service (see epd_gfx.h) - the two GATT services defined in
 * user_custs1_def.h with UUIDs matching the vendor's own esl_clock.php
 * tool. See PROTOCOL_NOTES.md for the full reverse-engineering background.
 */

#include "rwip_config.h"             // SW configuration
#include "gattc_task.h"
#include "app_api.h"
#include "user_empty_peripheral_template.h"
#include "custs1_task.h"
#include "user_custs1_def.h"
#include "epd_cmdparser.h"
#include "epd_gfx.h"
#include "epd_ssd1680.h"

void user_on_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param)
{
    default_app_on_connection(connection_idx, param);
}

void user_on_disconnect( struct gapc_disconnect_ind const *param )
{
    default_app_on_disconnect(param);
}

/* Running byte offset into epd_framebuffer for an in-progress image upload.
 * Reset to 0 whenever a write lands exactly at offset 0 (i.e. the app is
 * expected to always start a fresh image transfer from the top - matches
 * the vendor tool's own "send whole image every time" behavior; there's no
 * separate "begin transfer" command in the discovered protocol). */
static uint32_t s_img_write_offset = 0;

static void handle_cmd_write(struct custs1_val_write_ind const *msg)
{
    epd_cmd_process(msg->value, msg->length);
    /* The vendor firmware appears to push the framebuffer to the panel
     * after each applied command batch (see PROTOCOL_NOTES.md section 5
     * sample templates, which are sent as one big multi-line batch) rather
     * than after every single line, so do the same here: one refresh per
     * BLE write. */
    epd_display(epd_framebuffer);
}

static void handle_img_write(struct custs1_val_write_ind const *msg)
{
    uint32_t remaining = EPD_BUF_SIZE - s_img_write_offset;
    uint32_t n = (msg->length < remaining) ? msg->length : remaining;

    for (uint32_t i = 0; i < n; i++) {
        epd_framebuffer[s_img_write_offset + i] = msg->value[i];
    }
    s_img_write_offset += n;

    if (s_img_write_offset >= EPD_BUF_SIZE) {
        epd_display(epd_framebuffer);
        s_img_write_offset = 0;
    }
}

void user_catch_rest_hndl(ke_msg_id_t const msgid,
                          void const *param,
                          ke_task_id_t const dest_id,
                          ke_task_id_t const src_id)
{
    switch(msgid)
    {
        case GATTC_EVENT_REQ_IND:
        {
            // Confirm unhandled indication to avoid GATT timeout
            struct gattc_event_ind const *ind = (struct gattc_event_ind const *) param;
            struct gattc_event_cfm *cfm = KE_MSG_ALLOC(GATTC_EVENT_CFM, src_id, dest_id, gattc_event_cfm);
            cfm->handle = ind->handle;
            KE_MSG_SEND(cfm);
        } break;

        case CUSTS1_VAL_WRITE_IND:
        {
            struct custs1_val_write_ind const *msg_param =
                (struct custs1_val_write_ind const *)(param);

            switch (msg_param->handle)
            {
                case CMD_SVC_IDX_VAL:
                    handle_cmd_write(msg_param);
                    break;

                case IMG_SVC_IDX_VAL:
                    handle_img_write(msg_param);
                    break;

                default:
                    break;
            }
        } break;

        default:
            break;
    }
}

/// @} APP
