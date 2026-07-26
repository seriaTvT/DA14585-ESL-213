/**
 * epd_time.c - see epd_time.h.
 */

#include "epd_time.h"
#include "app_easy_timer.h"

#define TICK_10MS   100                 /* app_easy_timer units -> 1 s */

/* Days from 1970-01-01 to 2000-01-01. Our epoch is 2000, but the calendar
 * maths below is the standard days-since-1970 formulation, so we shift. */
#define DAYS_1970_TO_2000   10957

static volatile uint32_t   s_secs;      /* seconds since 2000-01-01 */
static bool                s_is_set;
static timer_hnd           s_tick = EASY_TIMER_INVALID_TIMER;
static epd_time_tick_cb_t  s_on_tick;

static void tick_cb(void)
{
    s_secs++;
    /* Re-arm first so a slow callback cannot skew the next interval. */
    s_tick = app_easy_timer(TICK_10MS, tick_cb);
    if (s_on_tick) {
        s_on_tick();
    }
}

void epd_time_init(epd_time_tick_cb_t on_tick)
{
    s_on_tick = on_tick;

    /* Always (re)arm rather than skipping when s_tick looks valid.
     * A handle can go stale - the SDK's app init tears down app timers, so a
     * tick armed before default_app_on_init() silently never fires while the
     * handle still reads non-zero. Re-arming here makes the call idempotent
     * and safe to repeat (e.g. from user_on_connection). */
    if (s_tick != EASY_TIMER_INVALID_TIMER) {
        app_easy_timer_cancel(s_tick);
        s_tick = EASY_TIMER_INVALID_TIMER;
    }
    s_tick = app_easy_timer(TICK_10MS, tick_cb);
}

void epd_time_set(uint32_t secs)
{
    s_secs = secs;
    s_is_set = true;
}

uint32_t epd_time_now(void)
{
    return s_secs;
}

bool epd_time_is_set(void)
{
    return s_is_set;
}

/* Howard Hinnant's civil_from_days: days since 1970-01-01 -> y/m/d.
 * Exact for the whole range we care about, no loops, no leap-year tables. */
static void civil_from_days(int32_t z, uint16_t *y, uint8_t *m, uint8_t *d)
{
    z += 719468;                                    /* shift epoch to 0000-03-01 */
    int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);            /* [0, 146096] */
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t  yr  = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  /* [0, 365]   */
    uint32_t mp  = (5 * doy + 2) / 153;                      /* [0, 11]    */
    uint32_t dd  = doy - (153 * mp + 2) / 5 + 1;             /* [1, 31]    */
    uint32_t mm  = mp + (mp < 10 ? 3 : -9);                  /* [1, 12]    */

    if (mm <= 2) {
        yr++;
    }
    *y = (uint16_t)yr;
    *m = (uint8_t)mm;
    *d = (uint8_t)dd;
}

void epd_time_get(epd_tm_t *tm)
{
    uint32_t secs = s_secs;
    uint32_t days = secs / 86400UL;
    uint32_t rem  = secs % 86400UL;

    tm->hour = (uint8_t)(rem / 3600);
    tm->min  = (uint8_t)((rem % 3600) / 60);
    tm->sec  = (uint8_t)(rem % 60);

    int32_t days1970 = (int32_t)days + DAYS_1970_TO_2000;
    civil_from_days(days1970, &tm->year, &tm->month, &tm->day);

    /* 1970-01-01 was a Thursday; 0 = Sunday. */
    tm->wday = (uint8_t)((days1970 + 4) % 7);
}
