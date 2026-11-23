/* alarm.c - see alarm.h */
#include "alarm.h"
#include "app_config.h"
#include "modbus_settings.h"

#include <stdbool.h>

/* Hysteresis: once on, an alarm needs the value back inside the limit by
 * `hyst` before it turns off. */
static bool temp_alarm(int16_t t, bool was_on)
{
	if (was_on) {
		return t > APP_ALARM_TEMP_HIGH_X10 - APP_ALARM_TEMP_HYST_X10 ||
		       t < APP_ALARM_TEMP_LOW_X10 + APP_ALARM_TEMP_HYST_X10;
	}
	return t > APP_ALARM_TEMP_HIGH_X10 || t < APP_ALARM_TEMP_LOW_X10;
}

static bool hum_alarm(int16_t h, bool was_on)
{
	if (was_on) {
		return h > APP_ALARM_HUM_HIGH_X10 - APP_ALARM_HUM_HYST_X10;
	}
	return h >= APP_ALARM_HUM_HIGH_X10;
}

uint16_t alarm_evaluate(const struct modbus_readings *r, uint16_t current)
{
	uint16_t out = current;

	for (int i = 0; i < MODBUS_NUM_SENSORS; i++) {
		if (r->status & (1U << i)) {
			continue; /* read error: keep the state, the value is stale */
		}
		uint16_t tbit = 1U << (MODBUS_COIL_TEMP_BASE + i);
		uint16_t hbit = 1U << (MODBUS_COIL_HUM_BASE + i);

		out = temp_alarm(r->temp_x10[i], current & tbit) ? (out | tbit) : (out & ~tbit);
		out = hum_alarm(r->hum_x10[i], current & hbit) ? (out | hbit) : (out & ~hbit);
	}
	return out;
}
