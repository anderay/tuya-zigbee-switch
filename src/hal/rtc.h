#pragma once
#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} hal_rtc_time_t;

void hal_rtc_init(void);
void set_rtc_seconds(uint32_t utc_time);
uint32_t get_rtc_seconds(void);
int get_rtc_time(hal_rtc_time_t *time);
int day_of_year(const hal_rtc_time_t *time);
