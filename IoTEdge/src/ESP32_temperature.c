#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>
#include "ESP32_temperature.h"

int ESP32_temperature_initialize(void){
    const struct device *temperature_sensor =
        DEVICE_DT_GET(DT_ALIAS(die_temp0));
    struct sensor_value temperature;

    if (!device_is_ready(temperature_sensor)) {
        printk("Internal temperature sensor is not ready\n");
        return 0;
    }

    while (1) {
        if (sensor_sample_fetch(temperature_sensor) == 0 &&
            sensor_channel_get(temperature_sensor, SENSOR_CHAN_DIE_TEMP,
                               &temperature) == 0) {
            printk("Internal temperature: %d.%02d C\n",
                temperature.val1, temperature.val2 / 10000);
        } else {
            printk("Failed to read internal temperature\n");
        }

        k_sleep(K_SECONDS(15));
    }
}
