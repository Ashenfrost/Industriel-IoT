/* alarm.h - alarm decisions for the SimPLC's coils.
 *
 * The Edge decides, the SimPLC acts: the result is a bitmask of the SimPLC's
 * coils (MODBUS_COIL_TEMP_BASE + i, MODBUS_COIL_HUM_BASE + i), which drive
 * its buzzer and lamp. Limits and hysteresis are in app_config.h.
 */
#pragma once

#include <stdint.h>
#include "modbus_client.h"

/* New coil mask from a reading and the current coils (needed for the
 * hysteresis). A sensor with a read error (status bit set) keeps its
 * current alarm state: an old value must neither raise nor clear an alarm. */
uint16_t alarm_evaluate(const struct modbus_readings *r, uint16_t current);
