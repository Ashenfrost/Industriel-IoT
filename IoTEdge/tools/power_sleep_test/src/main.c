/* power_sleep_test - steps the ESP32-C6 and the SIM7080G through their
 * power states, one phase at a time, for PPK2 measurements (test 4).
 *
 * The phase number is shown in binary on the PPK2 logic port
 * (D0 = bit 0, D1 = bit 1, D2 = bit 2; GPIO18/19/20), so every state can be
 * read off under the current plot. All markers low = deep sleep (or boot).
 *
 *   1  ESP CPU busy (100 % load)            modem registered, idle
 *   2  ESP idle, no light sleep (PM locked) modem registered, idle
 *   3  ESP light sleep                      modem registered, idle
 *   4  ESP light sleep                      modem radio off (AT+CFUN=0)
 *   5  ESP idle, no light sleep             modem radio off
 *   -  ESP deep sleep (markers low)         modem radio off
 *   ... wakes by timer -> reboots -> starts again at phase 1
 *
 * Phase length: PHASE_S. Deep sleep length: DEEP_SLEEP_S.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#include <esp_rom_sys.h>
#include <soc/reset_reasons.h>

#include "modem.h"
#include "at_parser.h"


#define PHASE_S       20
#define DEEP_SLEEP_S  30

#define USER_NODE DT_PATH(zephyr_user)
static const struct gpio_dt_spec marks[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, power_marker_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static const struct modem_cfg cfg = {
	.apn = "onomondo",
	.broker = "unused.invalid", /* no MQTT in this test */
	.port = 1883,
	.client_id = "power-sleep-test",
};

static bool pm_locked;

/* Keep the PM subsystem out of light sleep (phases 1, 2, 5 and all AT work:
 * the UART to the modem must keep running while we talk to it). */
static void light_sleep_allowed(bool allowed)
{
	if (allowed && pm_locked) {
		pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		pm_locked = false;
	} else if (!allowed && !pm_locked) {
		pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		pm_locked = true;
	}
}

static void show_phase(int phase)
{
	for (int b = 0; b < 3; b++) {
		gpio_pin_set_dt(&marks[b], (phase >> b) & 1);
	}
}

static void announce(int phase, const char *what)
{
	printk("PHASE %d: %s (%d s)\n", phase, what, PHASE_S);
	k_busy_wait(20000); /* let the log line leave before a sleep phase */
	show_phase(phase);
}

static void busy_for(int secs)
{
	int64_t end = k_uptime_get() + secs * 1000LL;

	while (k_uptime_get() < end) {
		k_busy_wait(1000);
	}
}

int main(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc_timer));
	int ret;

	light_sleep_allowed(false);
	for (size_t i = 0; i < ARRAY_SIZE(marks); i++) {
		gpio_pin_configure_dt(&marks[i], GPIO_OUTPUT_INACTIVE);
	}
	/* The RTC timer has to be a wakeup source for light sleep to end */
	if (pm_device_wakeup_is_capable(rtc)) {
		pm_device_wakeup_enable(rtc, true);
	}

	/* Reset register: 0x05 = woke from deep sleep. Anything else = power-on,
	 * reset button, flashing, or a crash/watchdog (which would be a bug). */
	int reason = (int)esp_rom_get_reset_reason(0);

	printk("power_sleep_test: reset reason 0x%02x (%s)\n", reason,
	       reason == RESET_REASON_CORE_DEEP_SLEEP ? "woke from deep sleep" : "other reset");

	/* Back from phases 4/5 + deep sleep the modem's radio (and SIM) is still
	 * off, and modem_init() checks the SIM. So turn the radio on first. */
	ret = at_init(uart);
	if (ret == 0 || ret == -EALREADY) {
		for (int i = 0; i < 15 && at_send("AT", NULL, 0, K_SECONDS(1)) != 0; i++) {
		}
		(void)at_send("AT+CFUN=1", NULL, 0, K_SECONDS(10));
		ret = modem_init(uart, &cfg);
	}
	if (ret == 0) {
		ret = modem_attach(120000);
	}
	printk("modem ready=%d (registered = 0)\n", ret);

	announce(1, "ESP busy, modem idle");
	busy_for(PHASE_S);

	announce(2, "ESP idle (no light sleep), modem idle");
	k_sleep(K_SECONDS(PHASE_S));

	announce(3, "ESP light sleep, modem idle");
	light_sleep_allowed(true);
	k_sleep(K_SECONDS(PHASE_S));
	light_sleep_allowed(false);

	show_phase(0);
	ret = at_send("AT+CFUN=0", NULL, 0, K_SECONDS(10));
	printk("AT+CFUN=0 -> %d\n", ret);

	announce(4, "ESP light sleep, modem radio off");
	light_sleep_allowed(true);
	k_sleep(K_SECONDS(PHASE_S));
	light_sleep_allowed(false);

	announce(5, "ESP idle (no light sleep), modem radio off");
	k_sleep(K_SECONDS(PHASE_S));

	printk("DEEP SLEEP for %d s (markers low), then reboot\n", DEEP_SLEEP_S);
	k_busy_wait(20000);
	show_phase(0);
	esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_S * 1000000ULL);
	sys_poweroff();
	return 0;
}
