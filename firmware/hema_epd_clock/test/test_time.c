/*
 * test_time.c - host-side check of the calendar maths in epd_time.c.
 *
 *   make -C firmware/hema_epd_clock/test
 *
 * ISO 8601 week numbering is the reason this exists. Weeks run Monday to
 * Sunday and belong to whichever year holds their Thursday, so the turn of the
 * year is full of cases a plausible-looking implementation gets wrong: the 1st
 * of January can sit in week 52 or 53 of the *previous* year, and some years
 * have 53 weeks. None of that is visible on the panel until the day it is
 * wrong, by which point nobody is looking - so it is pinned here instead.
 *
 * The expected values are GNU date's, i.e. `date -d <day> +%V/%G/%j`. The same
 * vectors are checked against the JS port in webui/test.mjs, which is what
 * keeps the preview and the panel agreeing.
 */
#include <stdio.h>
#include "epd_time.h"

static const struct {
    int y, m, d;            /* the date to set                */
    int week, wyear, yday;  /* what date(1) says about it      */
    const char *why;
} CASES[] = {
    {2026,  1,  1,  1, 2026,   1, "Thursday - week 1 starts on new year's day"},
    {2026, 12, 31, 53, 2026, 365, "2026 is a 53-week year"},
    {2027,  1,  1, 53, 2026,   1, "Friday - still last year's final week"},
    {2021,  1,  1, 53, 2020,   1, "same, across a leap year"},
    {2020, 12, 31, 53, 2020, 366, "leap year, 366 days"},
    {2019, 12, 30,  1, 2020, 364, "Monday - already next year's week 1"},
    {2024,  2, 29,  9, 2024,  60, "leap day"},
    {2026,  7, 26, 30, 2026, 207, "an unremarkable day, as a control"},
    {2000,  1,  1, 52, 1999,   1, "the tag's own epoch"},
    {2026, 12, 28, 53, 2026, 362, "Monday opening a 53rd week"},
};

/* Howard Hinnant's days_from_civil - the inverse of the civil_from_days() that
 * epd_time.c uses internally. Here only to turn a test date into the epoch
 * seconds the clock is set from. */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + (long)doe - 719468;
}

#define DAYS_1970_TO_2000 10957

static int check_calendar(void)
{
    int failed = 0;

    for (unsigned i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        long days = days_from_civil(CASES[i].y, CASES[i].m, CASES[i].d);
        epd_tm_t tm;

        epd_time_set((uint32_t)((days - DAYS_1970_TO_2000) * 86400L));
        epd_time_get(&tm);

        int bad = tm.year  != CASES[i].y     || tm.month != CASES[i].m
               || tm.day   != CASES[i].d     || tm.week  != CASES[i].week
               || tm.wyear != CASES[i].wyear || tm.yday  != CASES[i].yday;
        failed += bad;

        printf("%s  %04d-%02d-%02d  V=%02u G=%u j=%u  %s\n",
               bad ? "FAIL" : "ok  ",
               CASES[i].y, CASES[i].m, CASES[i].d,
               tm.week, tm.wyear, tm.yday, CASES[i].why);

        if (bad) {
            printf("      expected V=%02d G=%d j=%d, and %04d-%02d-%02d back\n",
                   CASES[i].week, CASES[i].wyear, CASES[i].yday,
                   CASES[i].y, CASES[i].m, CASES[i].d);
        }
    }
    return failed;
}

/* {L} drives nothing but text today, but it is the one field where a wrong
 * answer is both plausible and rare enough to survive casual use. */
static int check_month_lengths(void)
{
    static const int LEN[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    /* Both century rules are exercised, but only within the range the tag can
     * actually hold: the epoch is 2000 and the counter is unsigned, so 1900 is
     * not merely untested but unrepresentable - setting it wraps to some date
     * in the 2090s. 2000 (divisible by 400, leap) and 2100 (divisible by 100,
     * not leap) cover the rule between them. */
    static const int LEAP[] = { 2000, 2024, 2028 };
    static const int COMMON[] = { 2026, 2100 };
    int failed = 0;

    for (int m = 1; m <= 12; m++) {
        epd_tm_t tm;
        epd_time_set((uint32_t)((days_from_civil(2026, m, 1)
                                 - DAYS_1970_TO_2000) * 86400L));
        epd_time_get(&tm);
        if (tm.mdays != LEN[m]) {
            printf("FAIL  2026-%02d has %u days, expected %d\n",
                   m, tm.mdays, LEN[m]);
            failed++;
        }
    }

    for (unsigned i = 0; i < sizeof LEAP / sizeof *LEAP; i++) {
        epd_tm_t tm;
        epd_time_set((uint32_t)((days_from_civil(LEAP[i], 2, 1)
                                 - DAYS_1970_TO_2000) * 86400L));
        epd_time_get(&tm);
        if (tm.mdays != 29) {
            printf("FAIL  %d is a leap year, February got %u days\n",
                   LEAP[i], tm.mdays);
            failed++;
        }
    }

    for (unsigned i = 0; i < sizeof COMMON / sizeof *COMMON; i++) {
        epd_tm_t tm;
        epd_time_set((uint32_t)((days_from_civil(COMMON[i], 2, 1)
                                 - DAYS_1970_TO_2000) * 86400L));
        epd_time_get(&tm);
        if (tm.mdays != 28) {
            printf("FAIL  %d is not a leap year, February got %u days\n",
                   COMMON[i], tm.mdays);
            failed++;
        }
    }

    if (!failed) {
        printf("ok    month lengths, including the 100/400 leap-year rule\n");
    }
    return failed;
}

int main(void)
{
    int failed = check_calendar() + check_month_lengths();

    printf("\n%s\n", failed ? "FAILURES" : "all vectors match");
    return failed != 0;
}
