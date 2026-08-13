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
#include "adc.h"                     // adc_get_vbat_sample, for {VCC}
#include "battery.h"                 // battery_get_lvl, for {BAT}
#if (BLE_SUOTA_RECEIVER)
#include "app_suotar.h"              // suota_state, SUOTAR_START/SUOTAR_END
#endif

/* Scratch for a template restored from flash. Matches the parser's script
 * buffer; static because it is far too big for this callback's stack. */
#define EPD_RESTORE_MAX   3072   /* matches CMD_SCRIPT_MAX */

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

/* ...and this much more, once, if a line is only half received when the flush
 * fires. A gap with a dangling line is not the end of the batch - it is a gap
 * *inside a command*, and rendering then is not merely early: epd_cmd_run()
 * commits the partial line, so the broken command is what gets persisted, and
 * the good face on flash is gone.
 *
 * Seen on hardware as a calendar stored with its grid incomplete, its numbers
 * missing and its positions misaligned - a partial script, faithfully saved and
 * faithfully restored on every boot afterwards. The same defect was measured
 * directly on the nRF52811 tag, where a 144-byte push came back stored as
 * "TEXT(140,80,'GHOST TEST A',color" and "=0)".
 *
 * Granted once and then given up on, rather than waited for indefinitely: a
 * client that sends half a command and disconnects must still get its face
 * drawn, since a shelf label with no host in range has to display something. */
#define EPD_FLUSH_PARTIAL 200  /* -> 2000 ms */

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

/* Polls before giving up. This was 100 (5 s), sized against the high-res
 * panel's ~2 s refresh - but the low-res panel measured 67 polls (~3.4 s) on a
 * warm bench, leaving only 1.5x margin, and it loads a temperature-compensated
 * waveform from OTP so a cold tag legitimately refreshes slower still. A tag on
 * a chilled shelf would have tripped this while working perfectly.
 *
 * The only cost of a longer timeout is waiting longer to give up on a panel
 * that is genuinely stuck, which is rare and not time-critical; the cost of one
 * that is too short is a working panel declared dead. */
#define EPD_REFRESH_TIMEOUT 200    /* polls -> 10 s                           */

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

/* True while the panel is showing an uploaded image rather than the rendered
 * template. The two cannot coexist: they share one framebuffer, and the script
 * is replayed on every minute tick, so without this an image would be painted
 * over within the minute. Ownership changes only on a write from the host -
 * a completed image upload takes the panel, any command byte hands it back -
 * which keeps the rule short enough to state: whoever wrote last, wins.
 *
 * Not persisted. After a power cycle the tag comes back as a clock, because
 * the template is what survives in flash (see epd_store.h) and 4000 bytes of
 * image do not fit in its sector. */
static bool         s_image_mode;

#if (BLE_SUOTA_RECEIVER)
/* True for the whole of a SUOTA session. While it is set the panel is left
 * completely alone: no repaint is started, no in-flight one is watched, and the
 * SPI bus belongs to the flash rather than to the panel.
 *
 * The panel and the boot flash cannot share the bus. On variant B they share
 * CLK and MOSI outright, and P0_5 is the panel's D/C *and* the flash's MISO, so
 * there is no arrangement in which both are addressable at once - see
 * flash_bus_acquire() in platform/epd_store.c. A session lasts minutes and
 * ~230 blocks, and the kernel scheduler runs between blocks, so a minute tick
 * landing in one of those gaps would call epd_spi_claim() and take the bus back
 * underneath the transfer. Hence a session-long claim and this flag, rather
 * than the per-operation claim epd_store_save() gets away with. */
static bool         s_suota;

/* Whether the flash answered when the session claimed the bus. Nothing in the
 * firmware reads it; it is volatile so it can be read over SWD, which is the
 * only way to tell a transfer that failed on the bus hand-off from one that
 * failed on the radio. */
static volatile bool s_suota_bus_ok;
#endif

static void epd_poll_cb(void);

#if EPD_PARTIAL
/* When the panel was last swept clean. Zero at boot, which is correct rather than
 * lucky: the first paint has no shadow to diff against and is full anyway. */
static uint32_t s_last_full_sec;
#endif

/* Write the script to flash if it has changed since the last time.
 *
 * Called after rendering rather than before: a template that wedges the parser
 * should not be the one restored on the next boot. Safe to borrow the SPI bus
 * from either caller - the panel is either finished or was never started, and
 * epd_store_save() hands the bus back via epd_spi_claim(). It does block for the
 * flash erase and program, tens of milliseconds against a refresh's ~2 s. */
static void epd_persist_if_dirty(void)
{
    if (epd_cmd_take_dirty()) {
        epd_store_save(epd_cmd_script(), epd_cmd_script_len());
    }
}

/* Battery, for the {BAT} and {VCC} template variables.
 *
 * Off by default is not the right call here - unlike the panel temperature
 * there is no build where the ADC is absent - but it stays overridable so a
 * power measurement can take it out of the picture. */
#if !defined(EPD_BATT_READ)
#define EPD_BATT_READ 1
#endif

#if EPD_BATT_READ
static void sample_battery(void)
{
    /* Percentage first, and not only because it is the headline number:
     * battery_get_lvl() runs the ADC's offset calibration before it samples,
     * and adc_get_vbat_sample() below has no way to ask for that itself -
     * adc_offset_calibrate() is defined in adc_58x.c but declared in no
     * header. Sampling straight afterwards inherits the fresh calibration,
     * because the offset registers are not what adc_init() rewrites.
     *
     * BATT_CR2032 because that is the cell these tags ship with, and its
     * curve is a real discharge curve rather than a straight line between
     * two voltages. */
    uint8_t pct = battery_get_lvl(BATT_CR2032);

    /* adc_get_vbat_sample() returns the *sum of two* 10-bit conversions, so
     * full scale is 2046, not 1023 - the SDK's own comment there says to halve
     * it if 10-bit accuracy is enough. Single-ended with the attenuator in,
     * which puts full scale at 3.6 V, hence 2046 counts = 3600 mV and the
     * 1800/1023 below.
     *
     * The scale checks out against the SDK's own CR2032 curve for this chip,
     * which is written in raw counts where the DA14531's is in millivolts:
     * its 1705 and 1136 count breakpoints are that part's 3000 mV and 2000 mV,
     * and both give 568-569 counts per volt.
     *
     * Uncalibrated per-unit, so treat it as a reading to a few tens of mV
     * rather than a measurement. {BAT} is the number to show on a face; {VCC}
     * is for watching a cell age, which the percentage curve flattens out. */
    uint32_t sample = adc_get_vbat_sample(false);
    uint16_t mv = (uint16_t)((sample * 1800u) / 1023u);

    epd_cmd_set_batt(pct, mv);
}
#endif

/* Start a refresh of whatever is in the framebuffer, or queue one if the panel
 * is still busy with the last. */
static void epd_begin_refresh(epd_queued_t what)
{
#if (BLE_SUOTA_RECEIVER)
    /* A SUOTA session owns the SPI bus, so there is no way to reach the panel
     * and nothing useful to do but drop this. Dropped rather than queued: the
     * session ends with a repaint anyway, and by then the queue would only say
     * which of the ticks that passed happened to be last.
     *
     * This is the one funnel every repaint goes through - the minute tick, the
     * flush after a script push, the completed image upload, and the drain at
     * the end of a refresh - which is why the check lives here and not at each
     * of those. */
    if (s_suota) {
        return;
    }
#endif

    if (s_refreshing) {
        /* A queued script render supersedes a queued framebuffer one: it is
         * about to regenerate the framebuffer anyway. */
        if (what == EPD_Q_SCRIPT || s_queued == EPD_Q_NONE) {
            s_queued = what;
        }
        return;
    }

#if EPD_RESAMPLE_PER_REFRESH
    /* Before the script runs, not after: the waveform this picks is the one
     * this refresh will use, and epd_cmd_run() expands {T} from whatever the
     * reading leaves behind. Doing it here rather than once at init is what
     * keeps a tag that has been moved somewhere colder from driving every
     * pixel with the short warm waveform. */
    epd_resample_temperature();
#if EPD_TEMP_READ
    epd_cmd_set_temp(epd_temp_c);
#endif
#endif

#if EPD_BATT_READ
    sample_battery();
#endif

    if (what == EPD_Q_SCRIPT) {
        if (epd_cmd_script_len() == 0) {
            return;                   /* nothing configured yet */
        }
        epd_cmd_run();
    }

#if EPD_PARTIAL
    /* Sweep the panel clean on a clock as well as on a count. The driver's run
     * limit counts partials, which says nothing about how long they took - see
     * EPD_FULL_MAX_SECS. Owned here rather than in the driver because time is the
     * app's business and the driver has no clock.
     *
     * Unsigned on purpose: a TIME() sync that moves the clock backwards makes this
     * difference enormous and forces one full refresh. That is the harmless
     * direction, and it needs no special case. */
    if (epd_time_now() - s_last_full_sec >= EPD_FULL_MAX_SECS) {
        epd_display_forget();
    }
#endif

    epd_paint_t painted = epd_display_start(epd_framebuffer);

#if EPD_PARTIAL
    if (painted == EPD_PAINT_FULL) {
        s_last_full_sec = epd_time_now();
    }
#endif

    if (painted == EPD_PAINT_NONE) {
        /* Nothing was sent, because the frame already matches the glass. BUSY
         * will never rise, so arming the poll timer would burn the whole
         * EPD_REFRESH_TIMEOUT before concluding the panel had died.
         *
         * The housekeeping a finished refresh normally does still has to happen,
         * though: a client can push a script that renders to the same pixels -
         * a reworded comment, different whitespace, a face rebuilt from the same
         * parts - and that script must still reach flash. Skipping the refresh
         * is not a reason to skip persisting it.
         *
         * Nothing to drain from s_queued here: it is only ever set while
         * s_refreshing is true, and reaching this line means it was false. */
        epd_persist_if_dirty();
        return;
    }

    s_refreshing = true;
    s_poll_count = 0;
    s_poll_timer = app_easy_timer(EPD_POLL_DELAY, epd_poll_cb);
}

/* Which repaint window the clock is in now.
 *
 * Counted from the epoch rather than from the last repaint, and the epoch is
 * midnight-aligned, so the boundaries land where a reader expects: EVERY(60)
 * repaints on the hour and EVERY(1440) at midnight - not one interval after
 * whenever the face happened to be pushed. Measuring elapsed time instead
 * would drift to that arbitrary instant, which is the behaviour this avoids. */
static uint32_t s_last_slot = 0xFFFFFFFFu;

static uint32_t epd_slot_now(void)
{
    return (epd_time_now() / 60u) / epd_cmd_every_min();
}

/* Render the stored script and push it to the panel. */
static void epd_render_now(void)
{
    epd_begin_refresh(EPD_Q_SCRIPT);

    /* After the render, not before: epd_begin_refresh() is what runs the
     * script, and EVERY() only takes its new value once it has.
     *
     * Recording it here rather than in the tick is what stops a push costing
     * two refreshes. The tick's idea of the current slot is in the *previous*
     * face's units - a push that changes EVERY() changes the slot number out
     * of all recognition - so without this the next tick saw a change and
     * repainted a second time, a second after the push had already painted. */
    s_last_slot = epd_slot_now();
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

#if EPD_PARTIAL
    /* A refresh that ran out of polls did not necessarily paint anything, so the
     * driver's idea of what the glass holds is no longer trustworthy - and a
     * partial refresh diffed against a frame that was never displayed would leave
     * stale rows untouched forever. Force the next paint to be a full one.
     *
     * Cheap insurance: the only cost of being wrong here is one slow refresh. */
    if (s_poll_count >= EPD_REFRESH_TIMEOUT) {
        epd_display_forget();
    }
#endif

    epd_persist_if_dirty();

    if (s_queued != EPD_Q_NONE) {
        epd_queued_t next = s_queued;
        s_queued = EPD_Q_NONE;
        epd_begin_refresh(next);
    }
}

/* Whether this batch has already been granted the extra grace above, so it gets
 * one and not one per fragment. */
static bool s_flush_waited;

static void epd_flush_cb(void)
{
    s_flush_timer = EASY_TIMER_INVALID_TIMER;

    if (epd_cmd_line_pending() && !s_flush_waited) {
        s_flush_waited = true;
        s_flush_timer = app_easy_timer(EPD_FLUSH_PARTIAL, epd_flush_cb);
        return;
    }
    s_flush_waited = false;

#if EPD_PARTIAL
    /* A pushed face always paints fully.
     *
     * This is the one place that knows a refresh came from a host write rather
     * than from the minute tick, and the two want different things. A tick is an
     * update to a picture already on the glass, which is exactly what a partial
     * is for. A push is a NEW picture, and the author is looking at the tag while
     * it lands - so it should arrive clean rather than as a partial over whatever
     * was there, however few rows happen to differ.
     *
     * The row count cannot stand in for this. A new face can be a small edit to
     * the previous one and produce a tiny band, which is precisely when a partial
     * looks worst: a deliberate change rendered faintly on top of the old one. */
    epd_display_forget();
#endif

    epd_render_now();
}

/* Called once per second by the time base. A full refresh takes ~2 s, so we
 * only repaint when the displayed minute actually changes - re-rendering
 * every second would leave the panel permanently mid-refresh.
 *
 * A face can ask for less than that with EVERY(n). Once a minute is right for
 * a clock and pure waste for anything without minutes on it: the panel refresh
 * is the most expensive thing this tag does, and a calendar spends 1439 of
 * every 1440 repaints redrawing the same pixels. */
static void epd_on_second(void)
{
    /* EVERY() is applied while the script runs, so the interval reads 1 until
     * the first repaint has happened. That costs one extra repaint on a face
     * asking for fewer, and the alternative - not repainting until we know how
     * often to repaint - never starts at all. */
    if (epd_slot_now() == s_last_slot) {
        return;
    }

    /* Deliberately not recorded here. Every path that returns below declines
     * to repaint, and swallowing the slot change would mean the repaint it
     * declined never happens at all - the panel would sit on a stale face
     * until the *next* boundary. epd_render_now() records it, so only a
     * repaint that actually happened counts as having served this slot. */

    /* An uploaded image is not a clock face and has no template behind it, so
     * there is nothing to re-render: running the script here would regenerate
     * the framebuffer and paint the picture away. Before this check an image
     * lasted only until the next minute boundary, which made the whole image
     * path close to useless - the upload worked, then silently reverted. */
    if (s_image_mode) {
        return;
    }

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
    /* Fresh grace for each new gap: more bytes arrived, so this is a different
     * pause from the one that may already have been forgiven. */
    s_flush_waited = false;
    s_flush_timer = app_easy_timer(EPD_FLUSH_DELAY, epd_flush_cb);
}

#if (BLE_SUOTA_RECEIVER)
/* --- SUOTA sessions -------------------------------------------------------
 *
 * Both halves are idempotent, because they are reached from two directions that
 * cannot see each other. app_suotar_stop() - and so SUOTAR_END - is called only
 * when the client ends the service cleanly or the image completes. A client that
 * simply drops the link mid-transfer never produces it, and that is the ordinary
 * case for a fleet update that goes wrong: out of range, flat battery, closed
 * laptop. Without the disconnect path below, such a session would hold the bus
 * and suppress repaints until the next power cycle, leaving a tag that
 * advertises perfectly and never updates its glass again.
 *
 * A failed transfer is otherwise harmless: the receiver erases the target bank's
 * header before writing anything and only marks it valid at the end, so an
 * abandoned bank is invalid and the bootloader ignores it. Proven on this
 * hardware - hema-local/re/type4/suota/README.md. */
static void epd_suota_begin(void)
{
    if (s_suota) {
        return;
    }
    s_suota = true;

    /* Abandon an in-flight refresh rather than wait for it. Waiting is not
     * available - this runs in a BLE callback and the first image block can
     * arrive tens of milliseconds later - and abandoning costs very little: once
     * Master Activation has been sent the controller drives the update by
     * itself with no further SPI traffic, so what is given up is only the
     * deep-sleep command afterwards. That costs power, not correctness. */
    if (s_poll_timer != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(s_poll_timer);
        s_poll_timer = EASY_TIMER_INVALID_TIMER;
    }
    s_refreshing = false;
    s_queued = EPD_Q_NONE;

#if EPD_PARTIAL
    /* We stopped watching a refresh part way through, so what the glass ended up
     * holding is no longer known. Saying so here is what stops the repaint after
     * the session diffing against a shadow that describes a frame nobody saw. */
    epd_display_forget();
#endif

    /* Recorded rather than acted on. The session cannot be refused from here -
     * the callback returns void and the SDK has already told the client the
     * service is open - and a flash that does not answer will fail the transfer
     * on its own, with an error the client sees. What matters is being able to
     * tell that apart afterwards from a transfer the radio dropped. */
    s_suota_bus_ok = epd_store_flash_claim();
}

static void epd_suota_end(void)
{
    if (!s_suota) {
        return;
    }
    epd_store_flash_release();
    s_suota = false;

    /* Repaint on the way out. Every tick during the session was dropped, so on a
     * clock the glass is now stale by however long the transfer took. Follows
     * whoever last owned the panel, the same rule the rest of the app uses: an
     * uploaded image is still in the framebuffer and should come back, otherwise
     * re-run the template.
     *
     * On a successful update this is wasted work, since the tag is about to
     * reboot into the new image. It is the abandoned sessions this is for. */
    epd_begin_refresh(s_image_mode ? EPD_Q_FRAMEBUFFER : EPD_Q_SCRIPT);
}

void on_suotar_status_change(const uint8_t suotar_event)
{
    if (suotar_event == SUOTAR_START) {
        epd_suota_begin();
    } else {
        epd_suota_end();
    }
}
#endif // (BLE_SUOTA_RECEIVER)

/* --- a device name that identifies the individual tag ----------------------
 *
 * Every tag used to advertise as "HemaEPD-Clock", which is fine with one on the
 * bench and useless with several: a scanner - and in particular the vendor's own
 * SUOTA app, which offers a list to pick from - shows several identical entries
 * and no way to tell which is the one in your hand. So the low three bytes of the
 * BD address go on the end: "HemaEPD-T4B-682F8D".
 *
 * The address is the tag's own and needs no storage of ours. gapm_get_bdaddr()
 * is a ROM function (0x07f185ed in da14585_symbols.txt) that returns what the
 * stack is actually advertising with, so the name cannot disagree with the
 * address a scanner shows beside it. Declared here rather than by including
 * gapm_util.h, which is a stack-internal header and not on this project's
 * include path.
 *
 * Why the name has to be patched on *every* advertising restart, rather than
 * once: app_easy_gap_undirected_advertise_start() sets its cached command back
 * to NULL after sending it, so the next start rebuilds the advertising data from
 * USER_DEVICE_NAME - the placeholder - and a one-off patch would survive exactly
 * one connection. Hence the whole advertising operation is ours; it is a
 * documented hook (default_operation_adv in user_callback_config.h), so nothing
 * is being subverted here. */
extern struct bd_addr *gapm_get_bdaddr(void);

/* Starts as the placeholder and keeps its length forever - see USER_DEVICE_NAME.
 * Not const: the last six characters are the point. */
static char s_dev_name[USER_DEVICE_NAME_LEN + 1] = USER_DEVICE_NAME;
static bool s_dev_name_ready;

static void user_dev_name_init(void)
{
    static const char hex[] = "0123456789ABCDEF";
    struct bd_addr *bd = gapm_get_bdaddr();
    char *p = &s_dev_name[USER_DEVICE_NAME_LEN - 6];

    s_dev_name_ready = true;

    if (bd == NULL) {
        return;                 /* leave the placeholder's zeroes visible */
    }
    /* bd_addr is little-endian, so addr[0] is the byte a scanner prints last.
     * Walking down from addr[2] puts them in the printed order. */
    for (int8_t i = 2; i >= 0; i--) {
        *p++ = hex[(bd->addr[i] >> 4) & 0x0Fu];
        *p++ = hex[bd->addr[i] & 0x0Fu];
    }
}

/* Overwrite the name inside one advertising payload, if it is in this one.
 *
 * Found by walking the AD structures rather than by assuming an offset: which of
 * the two payloads the SDK puts the name in depends on how much room is left in
 * each (app.c), and that depends on USER_ADVERTISE_DATA_LEN, which is not this
 * file's business. Only the length the SDK already reserved is written, so a
 * payload cannot be lengthened here. */
static void user_dev_name_patch(uint8_t *data, uint8_t len)
{
    uint8_t i = 0;

    while (i + 1u < len && data[i] != 0u) {
        uint8_t field = data[i];            /* length of type + payload */

        if (data[i + 1u] == GAP_AD_TYPE_COMPLETE_NAME && i + 1u + field <= len) {
            uint8_t n = field - 1u;
            if (n > USER_DEVICE_NAME_LEN) {
                n = USER_DEVICE_NAME_LEN;
            }
            memcpy(&data[i + 2u], s_dev_name, n);
            return;
        }
        i = (uint8_t)(i + field + 1u);
    }
}

void user_advertise_operation(void)
{
    struct gapm_start_advertise_cmd *cmd;

    if (!s_dev_name_ready) {
        user_dev_name_init();
    }

    cmd = app_easy_gap_undirected_advertise_get_active();
    if (cmd != NULL) {
        user_dev_name_patch(cmd->info.host.adv_data,
                            cmd->info.host.adv_data_len);
        user_dev_name_patch(cmd->info.host.scan_rsp_data,
                            cmd->info.host.scan_rsp_data_len);
    }

    /* Mirrors default_advertise_operation(), which this replaces. */
    if (user_default_hnd_conf.adv_scenario == DEF_ADV_WITH_TIMEOUT) {
        app_easy_gap_undirected_advertise_with_timeout_start(
            user_default_hnd_conf.advertise_period, NULL);
    } else {
        app_easy_gap_undirected_advertise_start();
    }
}

/* What a connected peer reads from the GAP Device Name characteristic. Without
 * this the SDK answers with the placeholder, so the name in a scan list and the
 * name after connecting would disagree - and the second is the one a phone
 * remembers. */
void user_on_get_dev_name(struct app_device_name *device_name)
{
    if (!s_dev_name_ready) {
        user_dev_name_init();
    }
    device_name->length = USER_DEVICE_NAME_LEN;
    memcpy(device_name->name, s_dev_name, USER_DEVICE_NAME_LEN);
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

#if (BLE_SUOTA_RECEIVER)
    /* End any SUOTA session the client did not end itself. The SDK only calls
     * app_suotar_stop() on a clean service exit or a completed image, so a
     * client that goes out of range mid-transfer leaves the session open -
     * holding the SPI bus and suppressing every repaint. This is the only thing
     * that gets the panel back without a power cycle. */
    epd_suota_end();

    /* Then serve a reboot the receiver asked for. It sets this and disconnects
     * rather than resetting under the client, so that the client learns the
     * image was accepted before the tag goes away. Ordering matters: the reset
     * never returns, so the bus hand-off above has to happen first - on a
     * successful update it is the repaint that is wasted, not the release. */
    if (suota_state.reboot_requested) {
        suota_state.reboot_requested = 0;
        platform_reset(RESET_AFTER_SUOTA_UPDATE);
    }
#endif

    default_app_on_disconnect(param);
}

static void handle_cmd_write(struct custs1_val_write_ind const *msg)
{
    /* Any command byte hands the panel back to the template - including a bare
     * TIME(), which is not much of a drawing command but does imply the host
     * wants a clock again. The flush below repaints regardless, so treating
     * this as anything subtler would only mean the image mode flag disagreed
     * with what is actually on the screen. */
    s_image_mode = false;

    /* And abandon any half-finished upload, for the same reason the disconnect
     * path does. The image protocol carries no offset, so a transfer that a
     * command interrupts cannot be resumed; leaving the offset stranded
     * mid-buffer would make the *next* upload write its first byte into the
     * middle of the framebuffer. A client that gives up on an image and sends a
     * template instead is the ordinary way to reach this, not an edge case. */
    s_img_write_offset = 0;

    epd_cmd_feed(msg->value, msg->length);
    epd_schedule_flush();
}

static void handle_img_write(struct custs1_val_write_ind const *msg)
{
    uint32_t remaining = epd_buf_size - s_img_write_offset;
    uint32_t n = (msg->length < remaining) ? msg->length : remaining;

    for (uint32_t i = 0; i < n; i++) {
        epd_framebuffer[s_img_write_offset + i] = msg->value[i];
    }
    s_img_write_offset += n;

    if (s_img_write_offset >= epd_buf_size) {
        /* Only a *complete* image takes the panel. A partial upload has left
         * the top of the framebuffer overwritten and the rest stale, so if the
         * client vanishes mid-transfer the right thing is to stay a clock and
         * let the next minute tick repaint over the damage. */
        s_image_mode = true;

#if EPD_PARTIAL
        /* Fully, for the same reason a pushed script paints fully - and more so
         * here: an uploaded image shares nothing with what came before, so a
         * partial would render a whole new picture through a waveform meant for
         * touching up an existing one. */
        epd_display_forget();
#endif

        /* Refresh what was just uploaded, not the script - re-running the
         * template would regenerate the framebuffer and discard the image. */
        epd_begin_refresh(EPD_Q_FRAMEBUFFER);
        s_img_write_offset = 0;
    }
}

/* Answer a read of the status characteristic.
 *
 * Built here and now rather than kept as a copy in the attribute database: the
 * report describes the last render, and a cached one would go stale exactly
 * when it matters - the minute tick re-runs the script without anyone writing
 * to the tag, so there would be no write to hang an update off. */
static void handle_status_read(struct custs1_value_req_ind const *msg,
                               ke_task_id_t const dest_id,
                               ke_task_id_t const src_id)
{
    struct custs1_value_req_rsp *rsp = KE_MSG_ALLOC_DYN(CUSTS1_VALUE_REQ_RSP,
                                                        prf_get_task_from_id(TASK_ID_CUSTS1),
                                                        dest_id,
                                                        custs1_value_req_rsp,
                                                        EPD_STATUS_LEN);
    rsp->conidx  = app_env[msg->conidx].conidx;
    rsp->att_idx = msg->att_idx;

    if (msg->att_idx == STATUS_IDX_VAL) {
        rsp->length = EPD_STATUS_LEN;
        rsp->status = ATT_ERR_NO_ERROR;
        epd_cmd_status(rsp->value);
    } else {
        /* Nothing else is marked PERM(RI), so this is unreachable short of a
         * database change - answer honestly rather than returning stale
         * bytes. */
        rsp->length = 0;
        rsp->status = ATT_ERR_APP_ERROR;
    }

    KE_MSG_SEND(rsp);
    (void)src_id;
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

        case CUSTS1_VALUE_REQ_IND:
        {
            struct custs1_value_req_ind const *msg_param =
                (struct custs1_value_req_ind const *)(param);

            handle_status_read(msg_param, dest_id, src_id);
        } break;

        default:
            break;
    }
}

/// @} APP
