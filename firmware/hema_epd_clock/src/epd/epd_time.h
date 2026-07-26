/**
 * epd_time.h - software time base for the tag.
 *
 * The DA14585 has NO RTC peripheral (the SDK's rtc driver is guarded by
 * __DA14531__), so wall-clock time is kept in software: a 1 Hz kernel timer
 * increments a counter that is set from the host over BLE.
 *
 * Epoch is 2000-01-01 00:00:00, matching the vendor's own {u}/{g} template
 * variables ("seconds since 2000-1-1", see function_doc_official.txt) so a
 * host tool written for the original firmware needs no conversion.
 *
 * Accuracy is that of the BLE stack's low-power clock and it does not survive
 * a power cycle, so the host is expected to re-sync on connect.
 */

#ifndef _EPD_TIME_H_
#define _EPD_TIME_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t year;   /* full year, e.g. 2026        */
    uint8_t  month;  /* 1-12                        */
    uint8_t  day;    /* 1-31                        */
    uint8_t  hour;   /* 0-23                        */
    uint8_t  min;    /* 0-59                        */
    uint8_t  sec;    /* 0-59                        */
    uint8_t  wday;   /* 0 = Sunday .. 6 = Saturday  */

    /* Calendar fields, derived from the above. Filled in by epd_time_get()
     * rather than computed at the point of use: they cost a few dozen cycles
     * once per repaint, and a face may reference several of them. */
    uint16_t yday;   /* day of year, 1-366          */
    uint8_t  mdays;  /* days in this month, 28-31   */
    uint8_t  week;   /* ISO 8601 week number, 1-53  */
    uint16_t wyear;  /* ISO week-numbering year     */
} epd_tm_t;

/** Called once per second from the tick timer, after the counter advances.
 *  Runs in kernel-timer context - keep it short. */
typedef void (*epd_time_tick_cb_t)(void);

/** Start the 1 Hz tick. Safe to call once from user_on_connection or boot. */
void epd_time_init(epd_time_tick_cb_t on_tick);

/** Set the clock. `secs` = seconds since 2000-01-01 00:00:00. */
void epd_time_set(uint32_t secs);

/** Seconds since 2000-01-01 00:00:00. */
uint32_t epd_time_now(void);

/** True once epd_time_set() has been called - lets the UI distinguish
 *  "00:00 because unsynced" from a genuine midnight. */
bool epd_time_is_set(void);

/** Broken-down local time. */
void epd_time_get(epd_tm_t *tm);

#endif // _EPD_TIME_H_
