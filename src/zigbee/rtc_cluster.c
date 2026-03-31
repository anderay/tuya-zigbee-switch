#include "rtc_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "hal/printf_selector.h"
#include "hal/zigbee.h"
#include "hal/rtc.h"
#include "hal/timer.h"

#define RTC_SYNC_TIMEOUT_MS       1500u
#define RTC_INIT_SYNC_INTERVAL_MS (10u * 1000u)
#define RTC_SYNC_INTERVAL_MS      (60u * 60u * 1000u)

static uint8_t  rtc_sync_request_pending = 0;
static uint8_t  rtc_sync_join_seen       = 0;
// static uint8_t  rtc_sync_startup_done    = 0;
static uint32_t rtc_sync_request_started_ms;

zigbee_rtc_cluster *rtc_cluster_by_endpoint[10];

static zigbee_rtc_cluster *rtc_first_cluster(void) {
    for (uint8_t i = 0; i < 10; i++) {
        if (rtc_cluster_by_endpoint[i]) {
            return rtc_cluster_by_endpoint[i];
        }
    }
    return NULL;
}

static void rtc_cluster_request_time_from_binding(zigbee_rtc_cluster *cluster) {
    static const uint8_t payload[2] = {
        (uint8_t)(ZCL_ATTR_RTC_UTC_TIME & 0xFF),
        (uint8_t)(ZCL_ATTR_RTC_UTC_TIME >> 8),
    };

    hal_zigbee_cmd cmd = {
        .endpoint            = cluster->endpoint,
        .profile_id          = ZCL_HA_PROFILE,
        .cluster_id          = ZCL_CLUSTER_RTC,
        .command_id          = ZCL_CMD_READ_ATTRIBUTES,
        .cluster_specific    = 0,
        .direction           = HAL_ZIGBEE_DIR_CLIENT_TO_SERVER,
        .disable_default_rsp = 1,
        .manufacturer_code   = 0,
        .payload             = payload,
        .payload_len         = sizeof(payload),
    };

    hal_zigbee_status_t st = hal_zigbee_send_cmd_to_coordinator(&cmd);
    if (st == HAL_ZIGBEE_OK) {
        rtc_sync_request_pending     = 1;
        rtc_sync_request_started_ms  = hal_millis();
        printf("RTC: time sync request sent\r\n");
    } else {
        printf("RTC: time sync request failed: %d\r\n", st);
    }
}

static void rtc_cluster_remote_attr_callback(uint8_t endpoint,
                                             uint16_t cluster_id,
                                             uint16_t attribute_id,
                                             uint8_t data_type,
                                             const uint8_t *value,
                                             uint8_t value_len) {
    if (cluster_id != ZCL_CLUSTER_RTC || attribute_id != ZCL_ATTR_RTC_UTC_TIME) {
        return;
    }
    if (data_type != ZCL_DATA_TYPE_UTC && data_type != ZCL_DATA_TYPE_UINT32) {
        return;
    }
    if (value == NULL || value_len < 4) {
        return;
    }

    zigbee_rtc_cluster *cluster = rtc_cluster_by_endpoint[endpoint];
    if (!cluster) {
        cluster = rtc_first_cluster();
    }
    if (!cluster) {
        return;
    }

    uint32_t utc_time = (uint32_t)value[0] |
                        ((uint32_t)value[1] << 8) |
                        ((uint32_t)value[2] << 16) |
                        ((uint32_t)value[3] << 24);

    rtc_cluster_set_time(cluster, utc_time);
    rtc_sync_request_pending = 0;
    // rtc_sync_startup_done    = 1;
    rtc_sync_request_started_ms = hal_millis();
    printf("RTC: synced from network, utc=%u\r\n", utc_time);
}

void rtc_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                uint16_t attribute_id) {
    zigbee_rtc_cluster *cluster = rtc_cluster_by_endpoint[endpoint];
    if (!cluster) return;

    if (attribute_id == ZCL_ATTR_RTC_UTC_TIME) {
        rtc_cluster_set_time(cluster, cluster->utc_time);
    }
}

void rtc_cluster_add_to_endpoint(zigbee_rtc_cluster *cluster,
                                 hal_zigbee_endpoint *endpoint) {
    rtc_cluster_by_endpoint[endpoint->endpoint] = cluster;
    cluster->endpoint = endpoint->endpoint;

    SETUP_ATTR(0, ZCL_ATTR_RTC_UTC_TIME, ZCL_DATA_TYPE_UTC, ATTR_WRITABLE,
               cluster->utc_time);
    SETUP_ATTR(1, ZCL_ATTR_RTC_TIME_STATUS, ZCL_DATA_TYPE_BITMAP8, ATTR_READONLY,
               cluster->time_status);
    SETUP_ATTR(2, ZCL_ATTR_RTC_TIME_ZONE, ZCL_DATA_TYPE_INT32, ATTR_WRITABLE,
               cluster->time_zone);

    endpoint->clusters[endpoint->cluster_count].cluster_id = ZCL_CLUSTER_RTC;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 3;
    endpoint->clusters[endpoint->cluster_count].attributes = cluster->attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server = 1;
    endpoint->clusters[endpoint->cluster_count].cmd_callback = NULL;
    endpoint->cluster_count++;

    hal_zigbee_register_on_remote_attribute_callback(
        rtc_cluster_remote_attr_callback);
}

void rtc_cluster_set_time(zigbee_rtc_cluster *cluster, uint32_t utc_time) {
    cluster->utc_time = utc_time;
    // Let platform update hardware RTC if available
    rtc_set_hw_time(utc_time);
    hal_zigbee_notify_attribute_changed(cluster->endpoint, ZCL_CLUSTER_RTC,
                                        ZCL_ATTR_RTC_UTC_TIME);
}

void rtc_cluster_tick(void) {
    uint32_t now = get_rtc_seconds();
    if (now == 0) return;
    for (uint8_t i = 0; i < 10; i++) {
        if (rtc_cluster_by_endpoint[i]) {
            rtc_cluster_by_endpoint[i]->utc_time = now;
        }
    }
}

void rtc_cluster_time_sync_tick(uint32_t now_ms) {
    zigbee_rtc_cluster *cluster = rtc_first_cluster();
    if (!cluster) {
        return;
    }

    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED) {
        rtc_sync_request_pending = 0;
        rtc_sync_join_seen       = 0;
        // rtc_sync_startup_done    = 0;
        return;
    }

    if (!rtc_sync_join_seen) {
        rtc_sync_join_seen = 1;
        // rtc_sync_startup_done = 0;
        // rtc_sync_request_pending = 0;
        rtc_cluster_request_time_from_binding(cluster);
        rtc_sync_request_started_ms = now_ms;
        return;
    }

    if (rtc_sync_request_pending &&
        (uint32_t)(now_ms - rtc_sync_request_started_ms) >= RTC_SYNC_TIMEOUT_MS) {
        rtc_sync_request_pending = 0;
        printf("RTC: time sync request timed out\r\n");
    }

    if (!rtc_sync_request_pending) {
        if (((0 == get_rtc_seconds()) && ((uint32_t)(now_ms - rtc_sync_request_started_ms) > RTC_INIT_SYNC_INTERVAL_MS))
            || ((uint32_t)(now_ms - rtc_sync_request_started_ms) > RTC_SYNC_INTERVAL_MS)) {
            rtc_cluster_request_time_from_binding(cluster);
            // rtc_sync_startup_done = 1;
            rtc_sync_request_started_ms = now_ms;
            // return;
        }
    }

    // if (!rtc_sync_request_pending &&
    //     (uint32_t)(now_ms - rtc_sync_request_started_ms) >= RTC_SYNC_INTERVAL_MS) {
    //     rtc_cluster_request_time_from_binding(cluster);
    //     rtc_sync_request_started_ms = now_ms;
    // }
}

// Weak default implementation
__attribute__((weak)) void rtc_set_hw_time(uint32_t utc_time) {
    set_rtc_seconds(utc_time);
}
