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
#include "app_easy_timer.h"
#include "epd_time.h"

/* --- deferred panel refresh ------------------------------------------------
 * A full refresh takes ~2 s, and a client sends a batch of drawing commands
 * as many small writes (an ATT write carries only MTU-3 bytes). Refreshing
 * per write would mean one 2 s refresh per fragment and a visibly flickering
 * panel, so instead each write (re)arms a short timer and the panel is pushed
 * once, EPD_FLUSH_DELAY after the last command byte arrives.
 *
 * This also keeps us compatible with the vendor's esl_clock.php, which just
 * streams its command list with no explicit "commit" command - the discovered
 * DSL has no such command to key off. */
#define EPD_FLUSH_DELAY   40   /* app_easy_timer units are 10 ms -> 400 ms */

static timer_hnd s_flush_timer = EASY_TIMER_INVALID_TIMER;

/* Render the stored script and push it to the panel. */
static void epd_render_now(void)
{
    if (epd_cmd_script_len() == 0) {
        return;                       /* nothing configured yet */
    }
    epd_cmd_run();
    epd_display(epd_framebuffer);
}

static void epd_flush_cb(void)
{
    s_flush_timer = EASY_TIMER_INVALID_TIMER;
    epd_render_now();
}

/* Called once per second by the time base. A full refresh takes ~2 s, so we
 * only repaint when the displayed minute actually changes - re-rendering
 * every second would leave the panel permanently mid-refresh. */
static void epd_on_second(void)
{
    static uint8_t last_min = 0xFF;
    epd_tm_t tm;

    epd_time_get(&tm);
    if (tm.min == last_min) {
        return;
    }
    last_min = tm.min;

    /* Don't fight an in-flight batch: if commands are still arriving the
     * flush timer is armed and will repaint shortly anyway. */
    if (s_flush_timer == EASY_TIMER_INVALID_TIMER) {
        epd_render_now();
    }
}

static void epd_schedule_flush(void)
{
    if (s_flush_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(s_flush_timer);
    }
    s_flush_timer = app_easy_timer(EPD_FLUSH_DELAY, epd_flush_cb);
}

void user_on_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param)
{
    /* Arm - don't perform - a script replace. A client is expected to send its
     * whole template, so its bytes must not land underneath the previous one;
     * but a client that connects and sends nothing must leave the current face
     * alone. epd_cmd_begin_batch() defers the clear to the first write. */
    epd_cmd_begin_batch();

    /* Start the 1 Hz software clock here, NOT from an app-init hook: a timer
     * armed during app init never fires (app_easy_timer returns a handle that
     * reads as valid, so the failure is silent - verified on hardware by
     * watching the seconds counter over SWD). Connect time is also the
     * natural place: the DA14585 has no RTC, so the clock is meaningless
     * until a host sets it with TIME() over this very connection.
     * epd_time_init() re-arms, so repeat connections are harmless. */
    epd_time_init(epd_on_second);
    default_app_on_connection(connection_idx, param);
}

void user_on_disconnect( struct gapc_disconnect_ind const *param )
{
    /* Deliberately keep the script. An electronic shelf label has to go on
     * displaying - and, since the script is replayed on the minute tick, go on
     * *updating* - long after the host that configured it has gone away.
     * Dropping it here left the tag frozen at the last-rendered minute:
     * epd_render_now() bails on an empty script, so the face simply stopped
     * (found on hardware after a 30-minute stall). */
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
    epd_cmd_feed(msg->value, msg->length);
    epd_schedule_flush();
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
