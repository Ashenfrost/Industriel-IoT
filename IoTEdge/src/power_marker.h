/* power_marker.h - phase markers for current measurements with a PPK2.
 *
 * Each marker drives one GPIO high while a phase runs. Wired to the PPK2's
 * logic port, the phases show up as digital channels right under the
 * current plot, so the current of each phase can be read off directly,
 * even with USB unplugged (no console during a measurement).
 *
 *   D0  GPIO18  SESSION  uplink session: attach, PDP, time, MQTT, publish
 *   D1  GPIO19  PUBLISH  one MQTT publish (AT+SMPUB) in progress
 *   D2  GPIO20  SAMPLE   main loop takes a sample
 *   D3  GPIO21  MODBUS   one Modbus TCP poll of the SimPLC
 *
 * Off (all calls compile to nothing) unless APP_POWER_MARKERS = 1.
 * Pins come from the zephyr,user node's power-marker-gpios property.
 */
#pragma once

#include <stdbool.h>
#include "app_config.h"

enum power_marker {
	PM_MARK_SESSION = 0,
	PM_MARK_PUBLISH = 1,
	PM_MARK_SAMPLE  = 2,
	PM_MARK_MODBUS  = 3,
	PM_MARK_COUNT
};

#if APP_POWER_MARKERS
int power_marker_init(void);
void power_marker_set(enum power_marker m, bool on);
#else
static inline int power_marker_init(void) { return 0; }
static inline void power_marker_set(enum power_marker m, bool on) { (void)m; (void)on; }
#endif
