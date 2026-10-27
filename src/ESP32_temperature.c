#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "ESP32_temperature.h"

LOG_MODULE_REGISTER(temperature, LOG_LEVEL_INF);

static const struct device *const temperature_sensor =
	DEVICE_DT_GET(DT_ALIAS(die_temp0));

int ESP32_temperature_initialize(void)
{
	if (!device_is_ready(temperature_sensor)) {
		LOG_ERR("Internal temperature sensor is not ready");
		return -ENODEV;
	}
	return 0;
}

int ESP32_temperature_read(float *celsius)
{
	struct sensor_value temperature;
	int ret;

	ret = sensor_sample_fetch(temperature_sensor);
	if (ret == 0) {
		ret = sensor_channel_get(temperature_sensor, SENSOR_CHAN_DIE_TEMP,
					 &temperature);
	}
	if (ret) {
		LOG_ERR("Failed to read internal temperature: %d", ret);
		return ret;
	}

	*celsius = (float)sensor_value_to_double(&temperature);
	return 0;
}
