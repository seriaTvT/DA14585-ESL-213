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
/* No s_is_set. A "has the time been synced" flag was written here and read only
 * by epd_time_is_set(), which nothing called; the whole trio went 2026-08-13.
 * Nothing needs it: the tag shows 00:00 before a TIME() sync because s_secs
 * starts at zero, which is behaviour that falls out of the epoch rather than
 * something a flag has to gate. */
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
}

uint32_t epd_time_now(void)
{
    return s_secs;
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

static bool is_leap(uint16_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Days before the 1st of each month in a common year. */
static const uint16_t MDAYS_BEFORE[12] =
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
static const uint8_t  MDAYS[12] =
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Weekday of 31 December, as 0 = Sunday. A year has 53 ISO weeks exactly when
 * it starts on a Thursday, or is a leap year starting on a Wednesday - which
 * is what this expresses via the last day rather than the first. */
static uint8_t weeks_in_year(uint16_t y)
{
    uint32_t p = (y + y / 4 - y / 100 + y / 400) % 7;
    uint32_t q = (y - 1);
    q = (q + q / 4 - q / 100 + q / 400) % 7;
    return (p == 4 || q == 3) ? 53 : 52;
}

/* ISO 8601 week numbering. Weeks start on Monday and belong to whichever year
 * holds their Thursday, so the first days of January can fall in week 52/53 of
 * the *previous* year - hence the separate wyear, which is what makes a
 * "{V} {G}" pair correct across a new year rather than merely usually right. */
static void iso_week(epd_tm_t *tm)
{
    /* ISO counts Monday as 1 and Sunday as 7; epd_tm_t counts Sunday as 0. */
    uint8_t iso_wday = (tm->wday == 0) ? 7 : tm->wday;
    int32_t week = ((int32_t)tm->yday - iso_wday + 10) / 7;

    if (week < 1) {
        tm->wyear = tm->year - 1;
        tm->week  = weeks_in_year(tm->wyear);
    } else if (week > weeks_in_year(tm->year)) {
        tm->wyear = tm->year + 1;
        tm->week  = 1;
    } else {
        tm->wyear = tm->year;
        tm->week  = (uint8_t)week;
    }
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

    bool leap = is_leap(tm->year);
    tm->yday  = (uint16_t)(MDAYS_BEFORE[tm->month - 1] + tm->day
                           + ((leap && tm->month > 2) ? 1 : 0));
    tm->ydays = leap ? 366 : 365;
    tm->mdays = (uint8_t)(MDAYS[tm->month - 1]
                          + ((leap && tm->month == 2) ? 1 : 0));
    iso_week(tm);
}
