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
#include "epd_store.h"

/* Scratch for a template restored from flash. Matches the parser's script
 * buffer; static because it is far too big for this callback's stack. */
#define EPD_RESTORE_MAX   1024

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

/* --- asynchronous refresh --------------------------------------------------
 * The panel takes ~2 s to refresh, which is longer than BLE will wait for us.
 * Spinning on BUSY through that meant the tag dropped every connection about
 * 1.7 s after a push - and, because the minute tick below refreshes whether or
 * not anyone is connected, no client could hold a link for even a minute. That
 * is fine for a clock and fatal for anything else: SUOTA needs minutes of
 * stable link, and a 4000-byte image upload straddles a minute boundary often
 * enough to fail intermittently.
 *
 * So the refresh is driven as a state machine instead. epd_display_start()
 * triggers the panel and returns; a poll timer then checks BUSY every
 * EPD_POLL_DELAY, and between those callbacks the kernel scheduler runs
 * normally and the link stays up.
 *
 * Only one refresh can be in flight, so a request arriving during one is
 * queued rather than dropped - dropping it would lose whichever repaint came
 * second, which for the minute tick means a visibly stopped clock. */
#define EPD_POLL_DELAY      5      /* 10 ms units -> 50 ms between BUSY polls */
#define EPD_REFRESH_TIMEOUT 100    /* polls -> 5 s, ~2.5x a real refresh      */

/* What to repaint once the in-flight refresh finishes. The distinction matters
 * because the two sources disagree about who owns the framebuffer: a script
 * render regenerates it, while an image upload has already filled it and would
 * be overwritten by a re-run. */
typedef enum {
    EPD_Q_NONE = 0,
    EPD_Q_SCRIPT,        /* re-run the stored template, then refresh */
    EPD_Q_FRAMEBUFFER,   /* refresh what is already in the framebuffer */
} epd_queued_t;

static timer_hnd    s_poll_timer = EASY_TIMER_INVALID_TIMER;
static uint16_t     s_poll_count;
static bool         s_refreshing;
static epd_queued_t s_queued;

static void epd_poll_cb(void);

/* Start a refresh of whatever is in the framebuffer, or queue one if the panel
 * is still busy with the last. */
static void epd_begin_refresh(epd_queued_t what)
{
    if (s_refreshing) {
        /* A queued script render supersedes a queued framebuffer one: it is
         * about to regenerate the framebuffer anyway. */
        if (what == EPD_Q_SCRIPT || s_queued == EPD_Q_NONE) {
            s_queued = what;
        }
        return;
    }

    if (what == EPD_Q_SCRIPT) {
        if (epd_cmd_script_len() == 0) {
            return;                   /* nothing configured yet */
        }
        epd_cmd_run();
    }

    epd_display_start(epd_framebuffer);
    s_refreshing = true;
    s_poll_count = 0;
    s_poll_timer = app_easy_timer(EPD_POLL_DELAY, epd_poll_cb);
}

/* Render the stored script and push it to the panel. */
static void epd_render_now(void)
{
    epd_begin_refresh(EPD_Q_SCRIPT);
}

static void epd_poll_cb(void)
{
    s_poll_timer = EASY_TIMER_INVALID_TIMER;

    /* The timeout is a safety net for a panel that never releases BUSY (wrong
     * pin, inverted polarity, dead supply). Without it a bad build would leave
     * s_refreshing set forever and the clock would never repaint again. */
    if (epd_display_busy() && ++s_poll_count < EPD_REFRESH_TIMEOUT) {
        s_poll_timer = app_easy_timer(EPD_POLL_DELAY, epd_poll_cb);
        return;
    }
    s_refreshing = false;

    /* Persist after rendering, not before: a template that wedges the parser
     * should not be the one we restore on the next boot. Safe to borrow the
     * SPI bus here - the panel is done with it, and epd_store_save() hands it
     * back via epd_spi_claim(). This does block for the flash erase/program,
     * but that is tens of milliseconds against the refresh's ~2 s. */
    if (epd_cmd_take_dirty()) {
        epd_store_save(epd_cmd_script(), epd_cmd_script_len());
    }

    if (s_queued != EPD_Q_NONE) {
        epd_queued_t next = s_queued;
        s_queued = EPD_Q_NONE;
        epd_begin_refresh(next);
    }
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

void user_on_set_dev_config_complete(void)
{
    default_app_on_set_dev_config_complete();

    /* Boot-time face. This hook, not an app-init one: a timer armed during app
     * init never fires (see user_on_connection), whereas by the time the stack
     * has finished GAPM_SET_DEV_CONFIG the kernel timers work. It is also the
     * earliest point where a tag with no host in range can be made useful.
     *
     * Only seed when nothing is stored, so this stays a *default*: it must not
     * clobber a face a client pushed. Rendering here costs a ~2 s full refresh
     * before advertising is up, which is the same cost the old boot test
     * pattern had - only now it puts a clock on the panel instead of a grid. */
    if (epd_cmd_script_len() == 0) {
        static char restored[EPD_RESTORE_MAX];
        uint16_t restored_len = 0;

        /* Prefer a template a client saved earlier; the built-in face is only
         * the fallback for a tag that has never been configured (or whose
         * stored copy failed its CRC). */
        if (epd_store_load(restored, sizeof(restored), &restored_len)
                == EPD_STORE_OK) {
            epd_cmd_load_script(restored, restored_len);
        } else {
            epd_cmd_load_default();
        }
    }
    epd_time_init(epd_on_second);
    epd_render_now();
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

/* Running byte offset into epd_framebuffer for an in-progress image upload.
 * The protocol carries no offset - a client just streams the whole image from
 * the top, matching the vendor tool's own "send it all every time" behaviour -
 * so this is the only record of how far along a transfer is. */
static uint32_t s_img_write_offset = 0;

void user_on_disconnect( struct gapc_disconnect_ind const *param )
{
    /* Deliberately keep the script. An electronic shelf label has to go on
     * displaying - and, since the script is replayed on the minute tick, go on
     * *updating* - long after the host that configured it has gone away.
     * Dropping it here left the tag frozen at the last-rendered minute:
     * epd_render_now() bails on an empty script, so the face simply stopped
     * (found on hardware after a 30-minute stall). */

    /* An image transfer, on the other hand, is worthless once its client is
     * gone: the protocol carries no offset, so a half-finished upload cannot
     * be resumed. Leaving the offset stranded mid-buffer would make the *next*
     * transfer write its first byte into the middle of the framebuffer -
     * silent corruption rather than an honest failure. */
    s_img_write_offset = 0;

    default_app_on_disconnect(param);
}

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
        /* Refresh what was just uploaded, not the script - re-running the
         * template would regenerate the framebuffer and discard the image. */
        epd_begin_refresh(EPD_Q_FRAMEBUFFER);
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
