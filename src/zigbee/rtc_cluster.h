#ifndef _RTC_CLUSTER_H_
#define _RTC_CLUSTER_H_

#include "hal/zigbee.h"
#include <stdint.h>

typedef struct {
    uint8_t              endpoint;
    hal_zigbee_attribute attr_infos[4];
    uint32_t             utc_time;
    uint8_t              time_status;
    int32_t              time_zone;
} zigbee_rtc_cluster;

void rtc_cluster_add_to_endpoint(zigbee_rtc_cluster *cluster,
                                 hal_zigbee_endpoint *endpoint);

void rtc_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                uint16_t attribute_id);

void rtc_cluster_set_time(zigbee_rtc_cluster *cluster, uint32_t utc_time);

void rtc_cluster_tick(void);

void rtc_cluster_time_sync_tick(uint32_t now_ms);

// Weak hook to set hardware RTC; platform may override
__attribute__((weak)) void rtc_set_hw_time(uint32_t utc_time);

#endif
