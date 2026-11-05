#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_config.h"
#include "client_id.h"
#include "wifi_network.h"
#include "mqtt_client.h"
#include "time_sync.h"
#include "ESP32_temperature.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Zephyr app started");

    build_client_id_initialize();
    LOG_INF("client id: %s", build_client_id_get());

    if (network_initialize() != 0) {
        LOG_ERR("network init failed");
    }

    /* Real UTC timestamps for Sparkplug. If this fails, the MQTT thread
     * retries before each session. */
    time_sync_initialize();

    mqtt_client_initialize();

    if (ESP32_temperature_initialize() != 0) {
        LOG_ERR("no temperature sensor, nothing to sample");
        return 0;
    }

    /* Application thread: sample on a fixed interval and queue the result.
     * The MQTT thread batches and publishes, so a slow or broken network
     * never shifts the sampling interval. */
    int64_t next = k_uptime_get();

    while (1) {
        /* Stamped with uptime; converted to UTC when it is sent */
        struct sparkplug_sample s = { .timestamp_ms = (uint64_t)k_uptime_get() };

        if (ESP32_temperature_read(&s.temperature_c) == 0) {
            mqtt_client_queue_sample(&s);
            LOG_DBG("sample %d mC", (int)(s.temperature_c * 1000));
        }

        next += APP_SAMPLE_INTERVAL_S * MSEC_PER_SEC;
        k_sleep(K_TIMEOUT_ABS_MS(next));
    }

    return 0;
}
