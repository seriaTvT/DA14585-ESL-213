/*
 * stub/app_easy_timer.h - enough of the SDK to compile epd_time.c on the host.
 *
 * The calendar maths in epd_time.c is pure and worth testing away from
 * hardware, but the file also drives a kernel timer. Rather than splitting it
 * in two for the sake of a test, the timer API is stubbed: none of it is
 * reached by epd_time_set()/epd_time_get().
 */
#ifndef _STUB_APP_EASY_TIMER_H_
#define _STUB_APP_EASY_TIMER_H_

#include <stdint.h>

typedef uint8_t timer_hnd;
#define EASY_TIMER_INVALID_TIMER 0

static inline timer_hnd app_easy_timer(uint32_t delay, void (*cb)(void))
{
    (void)delay; (void)cb;
    return 1;
}

static inline void app_easy_timer_cancel(timer_hnd handle)
{
    (void)handle;
}

#endif
