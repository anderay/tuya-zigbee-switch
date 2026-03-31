#include "device_config/config_parser.h"
#include "device_config/device_type.h"
#include "device_config/nvm_items.h"
#include "device_config/reset.h"
#include "hal/nvm.h"
#include "hal/printf_selector.h"
#include "hal/rtc.h"
#include "hal/system.h"
#include "hal/timer.h"
#include "hal/zigbee.h"
#include "hal/zigbee_ota.h"
#include "zigbee/battery_cluster.h"
#include "zigbee/general_commands.h"
#include "zigbee/rtc_cluster.h"
#include "zigbee/relay_cluster.h"
#include "sun_time.h"
#ifdef END_DEVICE
#include "zigbee/poll_control_cluster.h"
#endif

void process_device_type_change() {
    // If device was updated from router to end device or vice versa,
    // we need to do a reset, as the network settings stored by SDK in NVM
    // are not compatible between these device types.
    // Read device type from NVM and compare with current configuration.
    enum device_type_t stored_device_type;
    hal_nvm_status_t   st =
        hal_nvm_read(NV_ITEM_DEVICE_TYPE, sizeof(stored_device_type),
                     (uint8_t *)&stored_device_type);

    if (st != HAL_NVM_SUCCESS) {
        // Unable to read device type from NVM, possibly first boot.
        stored_device_type = CURRENT_DEVICE_TYPE;
        hal_nvm_write(NV_ITEM_DEVICE_TYPE, sizeof(stored_device_type),
                      (uint8_t *)&stored_device_type);
        return;
    }
    if (stored_device_type != CURRENT_DEVICE_TYPE) {
        printf("Device type change detected: %d -> %d\r\n", stored_device_type,
               CURRENT_DEVICE_TYPE);
        // Device type has changed, update NVM and reset device.
        stored_device_type = CURRENT_DEVICE_TYPE;
        hal_nvm_write(NV_ITEM_DEVICE_TYPE, sizeof(stored_device_type),
                      (uint8_t *)&stored_device_type);
        // Perform a factory reset to clear incompatible network settings.
        hal_factory_reset();
        schedule_reboot(2000);
    }
}

#define RTC_TICK_INTERVAL_MS (1u * 1000u)
#define NIGHT_CHECK_INTERVAL_MS (1u * 60 * 1000u)
#define NIGHT_INIT_CHECK_INTERVAL_MS (5u * 1000u)

static bool     boot_announce_sent = false;
static uint32_t last_rtc_tick_ms   = 0;
static bool     is_night = false;
static uint32_t last_night_check_ms   = 0;

void app_init(void) {
    handle_version_changes();
    parse_config(); // Does most of the setup, including all callbacks
                    // registration
    hal_rtc_init();
    hal_zigbee_init_ota();
    init_global_attr_write_callback();

    process_device_type_change();

    uint32_t now_ms = hal_millis();
    last_rtc_tick_ms = now_ms;
    last_night_check_ms = now_ms - NIGHT_CHECK_INTERVAL_MS + NIGHT_INIT_CHECK_INTERVAL_MS; // Check night mode soon after boot
}

void app_task() {
#ifdef END_DEVICE
    poll_control_cluster_update();
#endif

    uint32_t now_ms = hal_millis();

    rtc_cluster_time_sync_tick(now_ms);

    if (now_ms - last_rtc_tick_ms > RTC_TICK_INTERVAL_MS) {
        last_rtc_tick_ms = now_ms;
        rtc_cluster_tick();
    }

    if (now_ms - last_night_check_ms > NIGHT_CHECK_INTERVAL_MS) {
        last_night_check_ms = now_ms;
        hal_rtc_time_t rtc_time;
        if (get_rtc_time(&rtc_time) == 0) {
            int day_time_result = is_daytime(&rtc_time);
            if (day_time_result >= 0) {
                if (day_time_result == 1 && is_night) {
                    printf("It's now daytime\r\n");
                    set_all_relays_state(false);
                } else if (day_time_result == 0 && !is_night) {
                    printf("It's now nighttime\r\n");
                    set_all_relays_state(true);
                }
                is_night = !day_time_result;
            }
        }
    }

    // TODO: add jitter to avoid all devices trying to join at once
    if (hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINED &&
        hal_zigbee_get_network_status() != HAL_ZIGBEE_NETWORK_JOINING) {
        hal_zigbee_start_network_steering();
    }
    if (!boot_announce_sent &&
        hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_JOINED) {
        hal_zigbee_send_announce();
        boot_announce_sent = true;
    }
}
