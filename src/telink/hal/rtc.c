#include "hal/rtc.h"
#pragma pack(push, 1)
#include "tl_common.h"
#pragma pack(pop)

#define RTC_TIMER_INDEX         TIMER_IDX_1

static volatile uint32_t rtc_base_utc_seconds;
static volatile uint32_t rtc_elapsed_seconds;
static volatile uint32_t rtc_next_deadline_tick;
static volatile uint8_t rtc_time_valid;
static volatile uint8_t rtc_timer_initialized;

static inline uint32_t rtc_tick_per_us(void)
{
#if defined(MCU_CORE_8258) || defined(MCU_CORE_8278)
    // On 8258/8278 the system timer (clock_time) runs at fixed 16 MHz.
    return 16;
#else
    // sysTimerPerUs is populated during platform init; keep a safe fallback.
    return sysTimerPerUs ? sysTimerPerUs : 16;
#endif
}

static inline uint32_t rtc_ticks_per_second(void)
{
    return 1000 * 1000 * rtc_tick_per_us();
}

static inline int rtc_time_reached(uint32_t now_tick, uint32_t deadline_tick)
{
    return (int32_t)(now_tick - deadline_tick) >= 0;
}

static void rtc_catch_up_locked(uint32_t now_tick)
{
    if (!rtc_time_valid || !rtc_next_deadline_tick) {
        return;
    }

    if (!rtc_time_reached(now_tick, rtc_next_deadline_tick)) {
        return;
    }

    uint32_t delta = now_tick - rtc_next_deadline_tick;
    uint32_t steps = (delta / rtc_ticks_per_second()) + 1;
    rtc_elapsed_seconds += steps;
    rtc_next_deadline_tick += steps * rtc_ticks_per_second();
}

static uint32_t rtc_remaining_us(uint32_t now_tick)
{
    uint32_t remaining_ticks = rtc_next_deadline_tick - now_tick;
    uint32_t tick_per_us = rtc_tick_per_us();
    uint32_t remaining_us = (remaining_ticks + tick_per_us - 1) / tick_per_us;
    return remaining_us ? remaining_us : 1;
}

static int rtc_timer_cb(void *arg)
{
    (void)arg;

    uint32_t now_tick = clock_time();
    rtc_catch_up_locked(now_tick);

    return (int)rtc_remaining_us(now_tick);
}

void hal_rtc_init(void)
{
    if (rtc_timer_initialized) {
        return;
    }

    uint32_t r = drv_disable_irq();

    if (!rtc_timer_initialized) {
        drv_hwTmr_init(RTC_TIMER_INDEX, TIMER_MODE_SCLK);
        if (drv_hwTmr_set(RTC_TIMER_INDEX, 1000 * 1000, rtc_timer_cb, NULL) == HW_TIMER_SUCC) {
            rtc_timer_initialized = 1;
            rtc_next_deadline_tick = clock_time() + rtc_ticks_per_second();
        }
    }

    drv_restore_irq(r);
}

void set_rtc_seconds(uint32_t utc_time) {
    hal_rtc_init();

    uint32_t r = drv_disable_irq();
    rtc_base_utc_seconds = utc_time;
    rtc_elapsed_seconds = 0;
    rtc_time_valid = (utc_time != 0);
    rtc_next_deadline_tick = clock_time() + rtc_ticks_per_second();
    drv_restore_irq(r);
}

uint32_t get_rtc_seconds(void) {
    hal_rtc_init();

    uint32_t r = drv_disable_irq();

    if (!rtc_time_valid) {
        drv_restore_irq(r);
        return 0;
    }

    rtc_catch_up_locked(clock_time());
    uint32_t now = rtc_base_utc_seconds + rtc_elapsed_seconds;

    drv_restore_irq(r);

    return now;
}

int get_rtc_time(hal_rtc_time_t *time)
{
    utcTime_t utc_time;
    uint32_t seconds = get_rtc_seconds();
    if (seconds == 0) {
        return -1; // No valid time available
    }
    ev_rtc_second2utc(&utc_time, seconds);
    time->year = utc_time.year;
    time->month = utc_time.month;
    time->day = utc_time.day;
    time->hour = utc_time.hour;
    time->min = utc_time.min;
    time->sec = utc_time.sec;
    return 0;
}

int day_of_year(const hal_rtc_time_t *time)
{
    // Calculate day of year using a common algorithm
    static const int days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int day_of_year = days_before_month[time->month - 1] + time->day;

    // Add one day if it's a leap year and we're past February
    if (time->month > 2) {
        if ((time->year % 4 == 0 && time->year % 100 != 0) || (time->year % 400 == 0)) {
            day_of_year += 1;
        }
    }

    return day_of_year;
}