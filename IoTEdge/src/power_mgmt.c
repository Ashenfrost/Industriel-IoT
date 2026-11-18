/* power_mgmt.c - see power_mgmt.h */
#include "power_mgmt.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

LOG_MODULE_REGISTER(power_mgmt, LOG_LEVEL_INF);

#if defined(CONFIG_PM)
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/atomic.h>
#endif

#if APP_LIGHT_SLEEP

#if !defined(CONFIG_PM)
#error "APP_LIGHT_SLEEP needs CONFIG_PM=y (prj.conf)"
#endif

static atomic_t counts[PWR_REASONS];
static atomic_t total;
static struct k_spinlock lock;

int power_mgmt_init(void)
{
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc_timer));

	if (!device_is_ready(rtc)) {
		LOG_ERR("rtc_timer not ready: no wakeup from light sleep");
		return -ENODEV;
	}
	if (pm_device_wakeup_is_capable(rtc)) {
		pm_device_wakeup_enable(rtc, true);
	}
	LOG_INF("light sleep enabled between work (console stops after the first sleep)");
	return 0;
}

void power_boot_done(void)
{
	power_unlock(PWR_BOOT);
}

void power_lock(enum power_reason r)
{
	K_SPINLOCK(&lock) {
		atomic_inc(&counts[r]);
		if (atomic_inc(&total) == 0) {
			pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		}
	}
}

void power_unlock(enum power_reason r)
{
	K_SPINLOCK(&lock) {
		if (atomic_get(&counts[r]) <= 0) {
			K_SPINLOCK_BREAK;
		}
		atomic_dec(&counts[r]);
		if (atomic_dec(&total) == 1) {
			pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
		}
	}
}

#else /* !APP_LIGHT_SLEEP */

int power_mgmt_init(void)
{
	/* The boot lock (below) is never released: no light sleep at all */
	LOG_INF("light sleep off (APP_LIGHT_SLEEP=0)");
	return 0;
}

void power_boot_done(void)
{
}

#endif /* APP_LIGHT_SLEEP */

#if defined(CONFIG_PM)
/* Block light sleep from the very start. With CONFIG_PM the idle thread may
 * light-sleep as soon as all threads wait, which already happens while
 * drivers start up, before main() runs. One such sleep kills the USB console
 * for good (the rest of the firmware runs on), so a lock taken in main() was
 * too late. Released by power_boot_done() with APP_LIGHT_SLEEP, kept for good
 * without it. */
static int power_boot_lock(void)
{
#if APP_LIGHT_SLEEP
	power_lock(PWR_BOOT);
#else
	pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
#endif
	return 0;
}
SYS_INIT(power_boot_lock, PRE_KERNEL_1, 0);
#endif
