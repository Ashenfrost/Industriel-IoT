/* power_mgmt.h - light sleep between work, and who may block it.
 *
 * With APP_LIGHT_SLEEP = 1 (CONFIG_PM) Zephyr puts the ESP32-C6 into light
 * sleep by itself whenever every thread is waiting (k_sleep, semaphores...).
 * RAM, the sample queue and all thread state survive; the RTC timer wakes it.
 *
 * Some work must not be interrupted by light sleep: an active Wi-Fi
 * connection, a Modbus TCP read, a modem session (UART traffic). Such code
 * takes a lock for as long as it runs:
 *
 *     power_lock(PWR_WIFI);  ...  power_unlock(PWR_WIFI);
 *
 * Light sleep is allowed only when no lock is held. Locks are counted per
 * reason, so nested or repeated calls are fine. All calls are no-ops when
 * APP_LIGHT_SLEEP = 0.
 *
 * Note: the USB console stops working after the first light sleep (seen in
 * power_sleep_test). Build with -DAPP_LIGHT_SLEEP=0 to debug with logs.
 */
#pragma once

#include <stdbool.h>
#include "app_config.h"

enum power_reason {
	PWR_WIFI = 0,     /* Wi-Fi connecting or connected */
	PWR_MODBUS,       /* Modbus TCP transaction */
	PWR_UPLINK,       /* modem session (UART, PDP, MQTT) */
	PWR_BOOT,         /* start-up, until the first sample is queued */
	PWR_REASONS
};

/* Call once early in main(). With APP_LIGHT_SLEEP it enables the RTC timer
 * as wakeup source; without it, it blocks light sleep for good. */
int power_mgmt_init(void);

#if APP_LIGHT_SLEEP
void power_lock(enum power_reason r);
void power_unlock(enum power_reason r);
#else
static inline void power_lock(enum power_reason r) { (void)r; }
static inline void power_unlock(enum power_reason r) { (void)r; }
#endif
